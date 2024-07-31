#include "sd.h"
#include "spi.h"
#include "uart.h"

// Parallel byte update CRC7-CCITT algorithm.
// The result is the CRC7 result, left shifted over by 1
// which is perfect, since we append a 1 at the end anyway
uint8_t crc7(uint8_t prev, uint8_t in) {
    // CRC polynomial 0x89
    uint8_t remainder = prev ^ in;
    remainder ^= (remainder >> 4) ^ (remainder >> 7);
    remainder = (remainder << 1) ^ (remainder << 4);
    return remainder & 0xff;
}

// Need to check this. This could be wrong as well.
uint16_t crc16(uint16_t crc, uint8_t data) {
    // CRC polynomial 0x11021
    crc = (uint8_t)(crc >> 8) | (crc << 8);
    crc ^= data;
    crc ^= (uint8_t)(crc >> 4) & 0xf;
    crc ^= crc << 12;
    crc ^= (crc & 0xff) << 5;
    return crc;
}

// sd_cmd ------------------------------------------------------------
// Sends SD card command using SPI mode.
// This function:
// * Chooses the response length based on the input command
// * Makes use of SPI's full duplex. For every byte sent,
//   a byte is received. Thus for every byte sent as part of
//   a command, a useless byte must be read from the receive
//   FIFO.
// * Takes advantage of the Sifive SPI peripheral spec's
//   watermark and interrupt features to determine when a
//   transfer is complete. This should save on cycles since
//   no arbitrary delays need to be added.
uint64_t sd_cmd(uint8_t cmd, uint32_t arg, uint8_t crc) {
  uint8_t response_len;
  uint8_t i;
  uint64_t r;
  uint8_t rbyte;

  // Initialize the response with 0's.
  r = 0;

  // Choose response length based on cmd input.
  // Most commands return an R1 format response.
  switch (cmd) {
    case 8:
      response_len = R7_RESPONSE;
      break;
    case 12:
      response_len = R1B_RESPONSE;
    default:
      response_len = R1_RESPONSE;
      break;
  }

  // Make interrupt pending after response fifo receives the correct
  // response length.  Probably unecessary so let's wait and see what
  // happens.
  // write_reg(SPI_RXMARK, response_len);

  // Chip select must remain asserted during transaction
  if (cmd != SD_CMD_STOP_TRANSMISSION) {
    write_reg(SPI_CSMODE, SIFIVE_SPI_CSMODE_MODE_HOLD);
  }
  
  // Write all 7 bytes into transfer fifo
  // spi_sendbyte(0xff);
  spi_sendbyte(0x40 | cmd);
  spi_sendbyte(arg >> 24);
  spi_sendbyte(arg >> 16);
  spi_sendbyte(arg >> 8);
  spi_sendbyte(arg);
  spi_sendbyte(crc);

  // Wait for command to send
  // The Transfer IP bit should go high when the txFIFO is empty
  // while(!(read_reg(SPI_IP) & 1)) {}
  waittx();

  // Read the dummy rxFIFO entries to move the head back to the tail
  for (i = 0; i < 7; i++) {
    spi_readbyte();
  }

  // Send "dummy signals". Since SPI is duplex,
  // useless bytes must be transferred
  for (i = 0; i < response_len; i++) {
    spi_sendbyte(0xFF);
  }

  // Wait for transfer fifo again
  waittx();

  // Wait for actual response from SD card
  // All responses start with a 0. Output of SDCIn is high, unless
  // a message is being transferred.
  do {
    rbyte = spi_txrx(0xff);
  } while ( (rbyte & 0x80) != 0 );

  r = r | (rbyte << ((response_len - 1)*8));
  
  // Read rxfifo response
  for (i = 1; i < response_len; i++) {
    rbyte = spi_readbyte();
    r = r | (rbyte << ((response_len - 1 - i)*8));
  }

  if (cmd != 18) {
    write_reg(SPI_CSMODE, SIFIVE_SPI_CSMODE_MODE_AUTO);
  }
  return r;
} // sd_cmd

uint64_t sd_read64(uint16_t * crc) {
  uint64_t r;
  uint8_t rbyte;
  int i;

  for (i = 0; i < 8; i++) {
    spi_sendbyte(0xFF);
  }

  waittx();

  for (i = 0; i < 8; i++) {
    rbyte = spi_readbyte();
    *crc = crc16(*crc, rbyte);
    r = r | (rbyte << ((8 - 1 - i)*8));
  }

  return r;
}

// Utility defines for CMD0, CMD8, CMD55, and ACMD41
#define CMD0()   sd_cmd( 0, 0x00000000, 0x95) // Reset SD card into IDLE state
#define CMD8()   sd_cmd( 8, 0x000001aa, 0x87) // 
#define CMD55()  sd_cmd(55, 0x00000000, 0x65) //
#define ACMD41() sd_cmd(41, 0x40000000, 0x77) //

// init_sd: ----------------------------------------------------------
// This first initializes the SPI peripheral then initializes the SD
// card itself. We use the uart to display anything that goes wrong.
int init_sd(uint32_t freq, uint32_t sdclk){
  spi_init();

  uint64_t r;
  uint32_t newClockDiv;

  println("Initializing SD Card in SPI mode.");
  // This is necessary. This is the card's pre-init state initialization.
  write_reg(SPI_CSMODE, SIFIVE_SPI_CSMODE_MODE_OFF);
  for (int i = 0; i < 10; i++) {
    spi_txrx(0xff);
  }
  write_reg(SPI_CSMODE, SIFIVE_SPI_CSMODE_MODE_AUTO);
  
  // CMD0 --------------------------------------------------------------
  // Reset SD Card command
  // Initializes SD card into SPI mode if CS is asserted '0'
  // We expect to get the R1 response 0x01 which means that the
  // card has been put into the idle state.
  print_uart("CMD0: ");
  do {
    r = CMD0();
  } while ( r != 0x01 );
  println_with_r1("Success, r = 0x", r & 0xff);

  // CMD8 -------------------------------------------------------------
  // 
  print_uart("CMD8: ");
  r = CMD8();
  if ((r & 0x000000ff0000ffff) != 0x01000001aa) {
    println_with_r7("Failed, 0x", r);
  }
  println_with_r7("Success, 0x", r);

  // ACMD41 -----------------------------------------------------------
  print_uart("ACMD41: ");
  do {
    CMD55();
    r = ACMD41();
  } while (r == 0x1);
  println_with_r1("Success, r = 0x", r & 0xff);

  println_with_dec("New clock frequency: ", (uint64_t)sdclk);
  spi_set_clock(freq, sdclk);
  println("SD card is initialized.");
}

