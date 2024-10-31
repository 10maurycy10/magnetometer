#include <stdint.h>
#include <avr/io.h>
#include <avr/delay.h>
#include "fs/ff.h"
#include "fs/diskio.h"

#define PORTC_E_CARD (1 << 0)
#define PORTC_E_SENSOR (1 << 1)
#define PORTC_LED (1 << 2)
#define PORTC_DRIVE_COIL (1 << 3)

#define PORTA_CS (1 << 7)


///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Low level SD card driver, supports SDSC, SDHC, and SDXC cards that work   //
// with SD version 2 or higher. Uses a block size of 512 (0x200) bytes       //
// regardless of the SD card used. The flash inside usually has a block size //
// of 512 or higher, so this doesn't impact lifespan.                        //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

// SPI tranceive function
uint8_t sd_xfer(uint8_t data) {
	SPI0.DATA = data;
	while (~SPI0.INTFLAGS & 1 << 7) ;
	SPI0.INTFLAGS = 1 << 7;
	return SPI0.DATA;
}

// Flash LED to indicate problem with the card.
void sd_timeout() {
	while (1) {
		PORTC.OUT ^= 1 << 2;
		_delay_ms(100);
	}
}

// Read a R1 format response, the other formats are just R1 with some data 
// tacked on, which can be read with sd_xfer(). 
uint8_t sd_get_r1() {
	uint16_t count = 0;
	while (1) {
		uint8_t r1 = sd_xfer(0xff);
		if (r1 != 0xFF) return r1;
		if (count > 16000) sd_timeout();
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

	// CMD0
	// Reset the card to a known state
	PORTA.OUTCLR = PORTA_CS;
	_delay_ms(1);
	sd_command(0, 0, 0x94);
	_delay_ms(1);
	sd_get_r1();

	// CMD8
	// This is supposed to be the voltage check command, but newer cards
	// will not work if this is not run.
	sd_command(8, 0x1AA, 0x87);
	if (sd_get_r1() != 0x01) sd_timeout();
	if (sd_xfer(0xFF) != 0x00) sd_timeout();
	if (sd_xfer(0xFF) != 0x00) sd_timeout();
	if (sd_xfer(0xFF) != 0x01) sd_timeout();
	if (sd_xfer(0xFF) != 0xAA) sd_timeout();

	// Repeatedly try to initialize the card until it exits the idle state.
	// This always takes a few attemps, I don't really know why.
	int done = 0;
	int timeout = 1000; // 10 seconds
	while (!done) {
		// Give up if the card doesn't initialize
		timeout--;
		if (timeout == 0) sd_timeout(); 
		_delay_ms(1);
		
		// ACMD 41
		sd_command(55, 0x0, 0x65);
		sd_get_r1();
		sd_command(41, 0x40000000, 0x77);
		
		uint8_t status = sd_get_r1();
		if (status == 0x00) done = 1;
		else if (status == 0x01) continue;
		else sd_timeout(); // Broken or very old card. (before SD ver. 2)
	}

	// At this point we know that the card supports SD V2 or higher
	// Some cards use byte addressing by default, so we read the OCR
	// register to check.
	sd_command(48, 0x0, 0x65);	
	sd_get_r1(); // First byte is reserved
	uint8_t OCR[4];
	OCR[0] = sd_xfer(0xFF);
	OCR[1] = sd_xfer(0xFF);
	OCR[2] = sd_xfer(0xFF);
	OCR[3] = sd_xfer(0xFF);
	sd_xfer(0xFF); // Final byte is reseved
	// Bit 30 of the OCR is set for block addressed cards (HC/XC)
	uint8_t CCR = (OCR[0] >> 6) & 1;

	// Switch the byte addressed cards to block addressing, using the
	// same 512 byte block size as the always block addressed cards
	// for consistancy
	if (!CCR) {
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
	ADC0.MUXPOS = 23; // AIN 23 
	ADC0.MUXNEG = 22; // AIN 22
}

void adc_measure() {
	ADC0.COMMAND = 1;
}

int64_t measure() {
	// Turn on the amplifier
	PORTC.OUTSET = PORTC_E_SENSOR;
	_delay_ms(10);
	adc_measure();
	_delay_ms(10);
	
	// Run drive coil
	int64_t total_p0 = 0;
	int64_t total_p1 = 0;
	for (int i = 0; i < 128; i++) {
		PORTC.OUTSET = PORTC_DRIVE_COIL; // Toggle driver
		_delay_us(15);
		total_p1 += ADC0.RES;
		adc_measure();
		_delay_us(15);
		PORTC.OUTCLR = PORTC_DRIVE_COIL; // Toggle driver
		_delay_us(15);
		total_p0 += ADC0.RES;
		adc_measure();
		_delay_us(15);
	}

	// Turn off unnedded components
	PORTC.OUTCLR = PORTC_DRIVE_COIL | PORTC_E_SENSOR;
	return total_p1 - total_p0;
}

//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// Main loop.                                                               //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////

FATFS fs;
FIL fd;

uint32_t lines_written = 0;

void write_banner() {
	f_puts("\n,,Fluxgate datalogger: restarted, numbering will be reset.\n", &fd);
	f_sync(&fd);
}
void write_datapoint(int64_t measurement) {
	PORTC.OUTSET = 1 << 2;
	f_printf(&fd, "%ld,%lld,\n", lines_written, measurement); 
	f_sync(&fd);
	lines_written++;
	PORTC.OUTCLR = 1 << 2;
}

int main(void) {
	PORTC.DIRSET = 0xFF; // LED

	PORTA.DIRSET = 1 << 4 | 1 << 5 | 1 << 6 | 1 << 7; // Sd card spi pins
	SPI0.CTRLA = 1 << 5 | 0x3 << 1 | 1; // SPI: Master, max prescaler, enabled

	adc_setup();
	
	// Write the startup banner to the card
	sd_init();
	if (f_mount(&fs, "", 1)) sd_timeout();
	if (f_open(&fd, "/FLUXGATE.CSV", FA_WRITE | FA_OPEN_APPEND)) sd_timeout();
	write_banner();

	// Flash LED	
	while (1) {
		_delay_ms(200);
		write_datapoint(measure());
	}
	return 0;
}
