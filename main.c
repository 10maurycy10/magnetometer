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


// SD card

// SPI trancieve primitiv
uint8_t sd_xfer(uint8_t data) {
	SPI0.DATA = data;
	while (~SPI0.INTFLAGS & 1 << 7) ;
	SPI0.INTFLAGS = 1 << 7;
	return SPI0.DATA;
}

void sd_timeout() {
	while (1) {
		PORTC.OUT ^= 1 << 2;
		_delay_ms(10);
	}
}

uint8_t sd_get_r1() {
	uint16_t count = 0;
	while (1) {
		uint8_t r1 = sd_xfer(0xff);
		if (r1 != 0xFF) return r1;
		if (count > 16000) sd_timeout();
		count++; 
	}
}

void sd_command(uint8_t cmd, uint32_t arg, uint8_t crc) {
	sd_xfer(cmd|0x40);
	sd_xfer((uint8_t)(arg >> 24));
	sd_xfer((uint8_t)(arg >> 16));
	sd_xfer((uint8_t)(arg >> 8));
	sd_xfer((uint8_t)(arg));
	sd_xfer(crc|0x01);
}


void sd_init() {
	// Power on the card
	PORTC.OUTSET = PORTC_E_CARD;
	PORTA.OUTSET = PORTA_CS;
	_delay_ms(1);

	// Send 80 dummy clock cycles with the card deselected
	for (int i = 0; i < 10; i++) sd_xfer(0xff);
	_delay_ms(1);

	// Send the initialization command
	PORTA.OUTCLR = PORTA_CS;
	_delay_ms(1);
	sd_command(0, 0, 0x94);
	_delay_ms(1);
	sd_get_r1();

	// CMD8
	// This is the voltage check, but required on newer cards
	sd_command(8, 0x1AA, 0x87);
	if (sd_get_r1() != 0x01) sd_timeout();
	if (sd_xfer(0xFF) != 0x00) sd_timeout();
	if (sd_xfer(0xFF) != 0x00) sd_timeout();
	if (sd_xfer(0xFF) != 0x01) sd_timeout();
	if (sd_xfer(0xFF) != 0xAA) sd_timeout();

	// Card initialization loop
	int done = 0;
	while (!done) {
		// ACMD 41
		sd_command(55, 0x0, 0x65);
		sd_get_r1();
		sd_command(41, 0x40000000, 0x77);
		uint8_t status = sd_get_r1();
		if (status == 0x00) done = 1;
		else if (status == 0x01) continue;
		else sd_timeout(); // Broken or very old card.
		_delay_ms(1); // Wait for it to initialize
		
	}

	// At this point we know that the card supports SD V2 or higher
	// Read OCR for addressing information
	sd_command(48, 0x0, 0x65);	
	sd_get_r1(); // First byte is reserved
	uint8_t OCR[4];
	OCR[0] = sd_xfer(0xFF);
	OCR[1] = sd_xfer(0xFF);
	OCR[2] = sd_xfer(0xFF);
	OCR[3] = sd_xfer(0xFF);
	sd_xfer(0xFF); // Final byte is reseved
	// Bit 30 of the OCR is set for SDHC and XC cards
	uint8_t CCR = (OCR[0] >> 6) & 1;

	// Low capacity cards use byte addressing by default, switch to 512 byte blocks for consistancy.
	if (!CCR) {
		sd_command(16, 0x200, 0x0);
		sd_get_r1();
	}
}

void card_power_off() {
	// Specification says to give 8 clock cycles after finishing operations, more shoun't hurt.
	for (int i = 0; i < 10; i++) sd_xfer(0xFF);
	_delay_ms(1);
	// Actually power off card
	PORTC.OUTCLR = PORTC_E_CARD;
}

// These functions will be called from the filesystem library
DSTATUS disk_initialize (BYTE pdrv) {
	return 0;
};

void read_block(uint8_t* buff, uint32_t sector) {
	sd_command(17, sector, 0);
	sd_get_r1(); // Resonse
	
	if (sd_get_r1() != 0xfe) sd_timeout(); // Read token
	for (int i = 0; i < 512; i++) { // Read data
		buff[i] = sd_xfer(0xff);
	}
	
	sd_xfer(0xff); sd_xfer(0xff); // Discard CRC
}

// Read part of a block
DRESULT disk_read (BYTE drive, BYTE* buff, LBA_t sector, UINT count) {
	for (int i = 0; i < count; i++) {
		read_block(&buff[512*i], sector + i);
	}
	return 0;
};

void write_block(uint8_t* buff, uint32_t sector) {
	sd_command(24, sector, 0); // Write block command
	sd_get_r1(); // Read responce
	sd_xfer(0xff); // Give it a moment
	sd_xfer(0b11111110); // Start token
	
	for (int i = 0; i < 0x200; i++) {
		sd_xfer(buff[i]);
	}
	
	// Send dummy CRC
	sd_xfer(0xff); sd_xfer(0xff);
	// Wait for completion
	sd_get_r1();

	while (sd_xfer(0xff) == 0x00) ; 
}

int bytes_written = 0;
DRESULT disk_write (BYTE drive, const BYTE* buff, LBA_t sector, UINT count) {
	for (int i = 0; i < count; i++) {
		write_block(&buff[0x200*i], sector);
	}
	return 0;
};

DRESULT disk_ioctl (BYTE pdrv, BYTE cmd, void* buff) {
	if (cmd == GET_SECTOR_SIZE) *((WORD*)buff) = 0x200;
	if (cmd == GET_BLOCK_SIZE) *((DWORD*)buff) = 1;
	return 0;
}

DSTATUS disk_status (BYTE pdrv) {
	return 0; // Access is blocking, no need to check.
}

DWORD get_fattime (void) {
	return 0; // Don't have a clock ;(
}

// Fluxgate measurement

void measure() {
	// Turn on the amplifier
	PORTC.OUTSET = PORTC_E_SENSOR;
	_delay_ms(10);
	
	// Run drive coil
	for (int i = 0; i < 32; i++) {
		PORTC.OUT ^= PORTC_DRIVE_COIL; // Toggle driver
		_delay_us(5);
	}

	// TUrn off unnedded components
	PORTC.OUTCLR = PORTC_DRIVE_COIL | PORTC_E_SENSOR;
}

// Main

FATFS fs;
FIL fd;

int main(void) {
	PORTC.DIRSET = 0xFF; // LED

	PORTA.DIRSET = 1 << 4 | 1 << 5 | 1 << 6 | 1 << 7; // Sd card spi pins
	SPI0.CTRLA = 1 << 5 | 0x3 << 1 | 1; // SPI: Master, max prescaler, enabled
	
	sd_init(); // Setup the card

	// Write a test file
	if (f_mount(&fs, "", 1)) while (1);
	_delay_ms(100);
	if (f_open(&fd, "/TEXT.TXT", FA_WRITE | FA_CREATE_ALWAYS)) while (1);
	_delay_ms(100);
	int written = 0;
	if (f_write(&fd, "Hello world!", 12, &written)) while (1);
	_delay_ms(100);
	if (f_close(&fd)) while (1);
	_delay_ms(100);
	
	while (1) {
		PORTC.OUT ^= 1 << 2;
		_delay_ms(100);
	}
	return 0;
}
