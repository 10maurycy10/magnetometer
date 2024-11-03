#include <stdint.h>
#include <avr/io.h>
#include <avr/delay.h>
#include "fs/ff.h"
#include "fs/diskio.h"

// TODO
// Burst (high frequency) measurements

#define PORTC_E_CARD (1 << 0)
#define PORTC_E_SENSOR (1 << 1)
#define PORTC_LED (1 << 2)
#define PORTC_DRIVE_COIL (1 << 3)

#define PORTA_CS (1 << 7)

uint32_t log_interval = 10000; // ms
int oversampling_ratio = 47; // Chosen to null out 60 Hz interference

FATFS fs;
FIL fd;

void read_config() {
	FIL config;
	if (f_open(&config, "FLUXGATE.CFG", FA_READ)) return;

	int32_t value;
	uint16_t len;
	
	// Read log interval
	f_read(&config, &value, sizeof(int32_t), &len);
	if (len > 0) log_interval = value;
	
	// Reading oversampling ratio
	f_read(&config, &value, sizeof(int32_t), &len);
	if (len > 0) oversampling_ratio = value;
}

// Flash LED at 5 Hz indicate problem with uSD
void sd_timeout() {
	while (1) {
		PORTC.OUT ^= PORTC_LED;
		_delay_ms(100);
	}
}

// Flash LED 3 times to indicate saturation
void saturated(){
	for (int i = 0; i < 3; i++) {
		PORTC.OUTSET = PORTC_LED;
		_delay_ms(50);
		PORTC.OUTCLR = PORTC_LED;
		_delay_ms(50);
	}
}

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Low level memory card driver, supports MMC (untested) SDSC, SDHC, and     //
// SDXC cards. Always uses a block size of 512 (0x200) bytes regardless of   //
// the card used. The flash inside usually has a block size of 512 or higher,//
// so this doesn't impact lifespan.                                          //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

// SPI tranceive function
uint8_t sd_xfer(uint8_t data) {
	SPI0.DATA = data;
	while (~SPI0.INTFLAGS & 1 << 7) ;
	SPI0.INTFLAGS = 1 << 7;
	return SPI0.DATA;
}


// Read a R1 format response, the other formats are just R1 with some data 
// tacked on, which can be read with sd_xfer(). 
uint8_t sd_get_r1() {
	uint16_t count = 0;
	while (1) {
		uint8_t r1 = sd_xfer(0xff);
		if (r1 != 0xFF) return r1;
		if (count > 16000) sd_timeout();
		_delay_us(10);
		count++; 
	}
}

// Returns 1 if a valid, error free response is given, 0 on errors or timeout.
uint8_t sd_check_r1() {
	uint16_t count = 0;
	while (1) {
		uint8_t r1 = sd_xfer(0xff);
		if (r1 == 0x0 || r1 == 0x1) return 1;
		if (r1 != 0xff) return 0;
		if (count > 16000) return 0;
		_delay_us(10);
		count++; 
	}
}

// Send a command to the card.
// The CRC will be ingored once initialized for SPI mode, but a correct
// checksum is needed during initilization.
void sd_command(uint8_t cmd, uint32_t arg, uint8_t crc) {
	sd_xfer(cmd|0x40);
	sd_xfer((uint8_t)(arg >> 24));
	sd_xfer((uint8_t)(arg >> 16));
	sd_xfer((uint8_t)(arg >> 8));
	sd_xfer((uint8_t)(arg));
	sd_xfer(crc|0x01);
}

// Initialize the card.
// Use on start up and after sd_power_off() before reading or writing.
void sd_init() {
	uint8_t is_v2 = 0, is_byte_addressed = 0;

	// Power on the card
	PORTA.DIRSET = 1 << 4 | 1 << 5 | 1 << 6 | 1 << 7; 
	PORTC.OUTSET = PORTC_E_CARD;
	_delay_ms(10);
	PORTA.OUTSET = PORTA_CS;
	_delay_ms(10);

	// Send 80 clock cycles with the card deselected
	// This lets the card know we're ready to start sending data.
	for (int i = 0; i < 10; i++) sd_xfer(0xff);
	_delay_ms(1);

	// CMD0: Software reset
	PORTA.OUTCLR = PORTA_CS;
	_delay_ms(1);
	sd_command(0, 0, 0x94);
	_delay_ms(1);
	sd_get_r1();

	// CMD8: Voltage check
	// This will work on newer V2 cards, but will fail on V1 or MMC cards.
	sd_command(8, 0x1AA, 0x87);
	if (sd_check_r1()) {
		// If it worked, read the eched responce
		is_v2 = 1;
		if (sd_xfer(0xFF) != 0x00) sd_timeout();
		if (sd_xfer(0xFF) != 0x00) sd_timeout();
		if (sd_xfer(0xFF) != 0x01) sd_timeout();
		if (sd_xfer(0xFF) != 0xAA) sd_timeout();
	} else {
		// We have a V1 or MMC card, either way it uses byte addressing by default
		is_byte_addressed = 1;
	}

	// Use ACMD41 to initialize the card. This will fail on MMC cards.
	// This always takes a few attemps, I don't really know why.
	int done = 0;
	int timeout = 1000; // 1 second
	while (!done) {
		// Give up if the card doesn't initialize
		timeout--;
		if (timeout == 0) sd_timeout(); 
		_delay_ms(1);
		
		// ACMD 41. 
		sd_command(55, 0x0, 0x65);
		if (!sd_check_r1()) {
			// If ACMD 41 failed, the card only supports MMC
			// Initilization has to be done with CMD1.
			while (!done) {
				sd_command(1, 0x0, 0);
				if (sd_get_r1() == 0x01) {
					timeout--;
					if (timeout == 0) sd_timeout(); 
				} else {
					done = 1;
				}
			}
			// MMC initization done, no need to keep trying SD setup.
			break;
		}
		sd_command(41, 0x40000000, 0x77);
		
		uint8_t status = sd_get_r1();
		if (status == 0x00) done = 1;
		else if (status == 0x01) continue;
		else sd_timeout(); // Broken or very old card. (before SD ver. 2)
	}

	// Some V2 cards use byte addressing, we have to check.
	if (is_v2) {	
		sd_command(48, 0x0, 0x65);	
		sd_get_r1(); // First byte is reserved
		uint8_t OCR[4];
		OCR[0] = sd_xfer(0xFF);
		OCR[1] = sd_xfer(0xFF);
		OCR[2] = sd_xfer(0xFF);
		OCR[3] = sd_xfer(0xFF);
		sd_xfer(0xFF); // Final byte is reseved
		// Bit 30 of the OCR is set for block addressed cards (HC/XC)
		is_byte_addressed = !(OCR[0] >> 6) & 1;
	}

	// If the card supports byte addressing, set the block size to 512 for
	// consitancy with the always block addressed cards (SDHC and higher)
	if (is_byte_addressed) {
		sd_command(16, 0x200, 0x0);
		sd_get_r1();
	}
}

// Disconnect power from the SD card to improve battery life.
void sd_power_off() {
	// Make sure the card has time to finish operations
	for (int i = 0; i < 10; i++) sd_xfer(0xFF);
	_delay_ms(1);
	// Actually cut power
	PORTA.DIRCLR = 1 << 4 | 1 << 5 | 1 << 6 | 1 << 7; 
	_delay_ms(1);
	PORTC.OUTCLR = PORTC_E_CARD;
	_delay_ms(1);
}

// Low level read and write primitives
void read_block(uint8_t* buff, uint32_t sector) {
	// Send read command
	sd_command(17, sector, 0);
	sd_get_r1();
	
	// Recieve data
	if (sd_get_r1() != 0xfe) sd_timeout();
	for (int i = 0; i < 512; i++) {
		buff[i] = sd_xfer(0xff);
	}
	
	// Discard the CRC
	sd_xfer(0xff); sd_xfer(0xff);
}

void write_block(uint8_t* buff, uint32_t sector) {
	// Send write command
	sd_command(24, sector, 0);
	sd_get_r1();
	
	// Give it some time before sending data
	sd_xfer(0xff); 
	
	// Send the start token
	sd_xfer(0b11111110);
	
	// Send the data
	for (int i = 0; i < 0x200; i++) {
		sd_xfer(buff[i]);
	}
	
	// Send dummy CRC
	sd_xfer(0xff); sd_xfer(0xff);

	// Wait for completion
	sd_get_r1();
	while (sd_xfer(0xff) == 0x00) ; 
}

//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// Filesystem driver interface, called by the FatFs library in fs/.         //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////

// We'll manualy initialize the card before mounting
DSTATUS disk_initialize (BYTE pdrv) {return 0;}

// Multiblock read. We don't care about speed, so using a bunch of single
// block commands is just fine.
DRESULT disk_read (BYTE drive, BYTE* buff, LBA_t sector, UINT count) {
	for (int i = 0; i < count; i++) {
		read_block(&buff[512*i], sector + i);
	}
	return 0;
};


// Multiblock write. We don't care about speed, so using a bunch of single
// block commands is just fine.
DRESULT disk_write (BYTE drive, const BYTE* buff, LBA_t sector, UINT count) {
	for (int i = 0; i < count; i++) {
		write_block(&buff[0x200*i], sector);
	}
	return 0;
};

// This function tells the library the block size for reading (SECTOR_SIZE)
// and writing (BLOCK_SIZE, in multiples of SECTOR_SIZE)
DRESULT disk_ioctl (BYTE pdrv, BYTE cmd, void* buff) {
	if (cmd == GET_SECTOR_SIZE) *((WORD*)buff) = 0x200;
	if (cmd == GET_BLOCK_SIZE) *((DWORD*)buff) = 1;
	return 0;
}

// The read/write primitives are blocking, nothing to do here.
DSTATUS disk_status (BYTE pdrv) {
	return 0; 
}

// Without an RTC, we have no way of knowing the real time, so 
// just use an blatantly bogus date (1980)
DWORD get_fattime (void) {
	return 0; // don't have a clock :(
}

//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// Magnetic field measurement.                                              //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////


void adc_setup() {
	VREF.ADC0REF = 0x0; // Internal 1.024 V reference
	ADC0.CTRLA = 0x1 << 5 | 0x1; // Diff, enable
	ADC0.CTRLB = 0x0; // Single shot
	ADC0.CTRLC = 0x0; // Div/2
	ADC0.CTRLE = 0x0; // No comparitor
	ADC0.MUXPOS = 23; // AIN 23 
	ADC0.MUXNEG = 22; // AIN 22
}

// Two quick flashes then delay
void self_test_failure() {
	f_printf(&fd, ",,Self test failed. Giving up.\n"); 
	f_close(&fd);
	sd_power_off(); // Ensure that the log file is written
	while (1) {
		PORTC.OUTSET = PORTC_LED;
		_delay_ms(100);
		PORTC.OUTCLR = PORTC_LED;
		_delay_ms(100);
		PORTC.OUTSET = 1 << 2;
		_delay_ms(100);
		PORTC.OUTCLR = 1 << 2;
		_delay_ms(500);
	}
}

// Sanity check for the ciruict
// Measurements are taken multiple times to check for bad (high-z) connections.
void self_test() {
	adc_setup();
	PORTC.OUTSET = PORTC_E_SENSOR;
	_delay_ms(50);

	// Read voltage divider
	VREF.ADC0REF = 1; // 2.048 V reference
	ADC0.MUXPOS = 22; // AIN 22
	ADC0.MUXNEG = 0x40; // GND
	for (int i = 0; i < 50; i++) {
		ADC0.COMMAND = 1;
		while (ADC0.COMMAND) ;
	}
	int16_t vdiv = ADC0.RES;
	f_printf(&fd, "Vdiv,%d\n", vdiv); 

	// Read amplifier output
	VREF.ADC0REF = 1; // 2.048 V reference
	ADC0.MUXPOS = 23; // AIN 23
	ADC0.MUXNEG = 0x40; // GND
	for (int i = 0; i < 50; i++) {
		ADC0.COMMAND = 1;
		while (ADC0.COMMAND) ;
	}
	int16_t vamp = ADC0.RES;
	f_printf(&fd, "Vamp,%d\n", vamp); 
	
	// Read difference
	VREF.ADC0REF = 1; // 2.048 V reference
	ADC0.MUXPOS = 23; // AIN 23
	ADC0.MUXNEG = 22; // AIN 22
	for (int i = 0; i < 50; i++) {
		ADC0.COMMAND = 1;
		while (ADC0.COMMAND) ;
	}
	int16_t vdiff = ADC0.RES;
	f_printf(&fd, "Vdiff,%d\n", vdiff); 
	
	
	// Turn off the amplifier
	PORTC.OUTCLR = PORTC_E_SENSOR;
	
	// Fluxh file
	f_sync(&fd);

	// With a 2.048 volt reference and 2048 bins per vref, the output is will be in mV
	int32_t expected = 1560; 
	if (vdiv > (expected + 200) || vdiv < (expected - 200)) self_test_failure();
	if (vamp > (expected + 200) || vamp < (expected - 200)) self_test_failure();
	if (vdiff > 50 || vdiff < -50) self_test_failure();
}

int32_t measure() {
	// Reset ADC for 1 volt (.5 mV res) differential mode
	adc_setup();
	
	// Run drive coil
	int16_t p0 = 0, p1 = 0;
	for (int i = 10; i > 0; i--) {
		PORTC.OUT ^= 1 << 2;
		ADC0.COMMAND = 1;
		_delay_us(17 + 12);
		PORTC.OUTSET = PORTC_DRIVE_COIL;
		if (i == 5) p1 = 0;
		p1 += ADC0.RES;
		
		PORTC.OUT ^= 1 << 2;
		ADC0.COMMAND = 1;
		_delay_us(17+12);
		PORTC.OUTCLR = PORTC_DRIVE_COIL; 
		if (i == 5) p0 = 0;
		p0 += ADC0.RES;
	}
	
	return p1 - p0;
}

// Add up a bunch of measurements together to minize noise
int32_t oversample(int times) {
	int32_t acc = 0;
	for (int i = 0; i < times; i++) {
		acc += measure();
	}
	
	int32_t avg = (acc / times / 5);
	if (avg < 0) avg *= -1;
	if (avg > 1800) saturated();
	
	return acc;
}

//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// Main loop.                                                               //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////


uint32_t lines_written = 0;

void write_banner() {
	f_puts("\n,,Fluxgate datalogger: restarted.\n", &fd);
	f_printf(&fd, "Tlog,%ld\n", log_interval);
	f_printf(&fd, "OSR,%d\n", oversampling_ratio);
	f_sync(&fd);
}

void write_datapoint(int32_t measurement) {
	f_printf(&fd, "%ld,%ld,\n", lines_written, measurement); 
	f_sync(&fd);
	lines_written++;
}

int main(void) {
	PORTC.DIRSET = 0xFF; // LED

	PORTA.DIRSET = 1 << 4 | 1 << 5 | 1 << 6 | 1 << 7; // Sd card spi pins
	SPI0.CTRLA = 1 << 5 | 0x3 << 1 | 1; // SPI: Master, max prescaler, enabled

	adc_setup();
	
	// Mount the card, read config and open log file
	sd_init();
	if (f_mount(&fs, "", 1)) sd_timeout();
	read_config();
	if (f_open(&fd, "/FLUXGATE.CSV", FA_WRITE | FA_OPEN_APPEND)) sd_timeout();
	write_banner();

	// Run self test, this writes to the card
	self_test();

	// Max = ~33 seconds
	TCA0.SINGLE.PER = log_interval * 1959 / 1000;
	TCA0.SINGLE.CTRLA = 0b10001111; // clk_per/1024 = 1.953 kHz
	
	// Loggging loop
	while (1) {
		// Wait for timer to overflow
		while (~TCA0.SINGLE.INTFLAGS & 1) ;
		TCA0.SINGLE.INTFLAGS = 1;

		// Record field reading
		PORTC.OUTSET = PORTC_E_SENSOR;
		_delay_ms(10);
		write_datapoint(oversample(oversampling_ratio));
		PORTC.OUTCLR = PORTC_DRIVE_COIL | PORTC_E_SENSOR;
	}

	return 0;
}
