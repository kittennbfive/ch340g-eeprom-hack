#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <err.h>

#include <libusb-1.0/libusb.h>

/*
ch340g-eeprom

using libusb 1.0

version 0.1 - 26.01.2023

(c) 2023 by kittennbfive

AGPLv3+ and NO WARRANTY!
*/


/////////////////copied from (modified) driver/////////////////////////
//INPUTS
#define CH341_BIT_CTS 0x01
#define CH341_BIT_DSR 0x02
#define CH341_BIT_RI  0x04
#define CH341_BIT_DCD 0x08
#define CH341_BITS_MODEM_STAT 0x0f /* all bits */
//OUTPUTS
#define CH341_BIT_DTR (1 << 5)
#define CH341_BIT_RTS (1 << 6)

#define CH341_I2C_DELAY_US 10

#define CH341_EEPROM_SIG0 'C'
#define CH341_EEPROM_SIG1 'H'

#define CH341_SZ_IDENT_EEPROM 8 //ASCII-chars, without ending '\0'
///////////////////////////////////////////////////////////////////////

/* SCL: DTR (out) */
/* SDA: RTS (out) + CTS (in) */

static uint8_t mcr=CH341_BIT_DTR|CH341_BIT_RTS;

static uint8_t ch340g_get_pins(libusb_device_handle *dh)
{
	uint8_t data[2];
	int ret;
	ret=libusb_control_transfer(dh, 0x80|(0x02<<5)|0x00, 0x95, 0x0706, 0, data, 2, 50);
	if(ret<=0)
		errx(1, "libusb_control_transfer IN failed: %s", libusb_error_name(ret));
	
	return (~data[0])&CH341_BITS_MODEM_STAT;
}

static void ch340g_set_pins(libusb_device_handle *dh, uint8_t pins)
{
	int ret;
	ret=libusb_control_transfer(dh, 0x00|(0x02<<5)|0x00, 0xA4, pins, 0, NULL, 0, 50);
	if(ret<0)
		errx(1, "libusb_control_transfer OUT failed: %s", libusb_error_name(ret));
}


static bool ch340g_get_cts(libusb_device_handle *dh) //SDA in
{
	return !(ch340g_get_pins(dh)&CH341_BIT_CTS); //make logic positive
}

static void ch340g_set_dtr(libusb_device_handle *dh, const bool dtr) //SCL out
{
	if(dtr)
		mcr|=CH341_BIT_DTR;
	else
		mcr&=~CH341_BIT_DTR;
	
	ch340g_set_pins(dh, mcr);
}

static void ch340g_set_rts(libusb_device_handle *dh, const bool rts) //SDA out
{
	if(rts)
		mcr|=CH341_BIT_RTS;
	else
		mcr&=~CH341_BIT_RTS;
	
	ch340g_set_pins(dh, mcr);
}

static void ch340g_set_scl(libusb_device_handle *dh, const bool scl)
{
	ch340g_set_dtr(dh, scl);
	usleep(CH341_I2C_DELAY_US);
}

static void ch340g_set_sda(libusb_device_handle *dh, const bool sda)
{
	ch340g_set_rts(dh, sda);
	usleep(CH341_I2C_DELAY_US);
}

static bool ch340g_get_sda(libusb_device_handle *dh)
{
	return ch340g_get_cts(dh);
}

static void ch340g_i2c_init(libusb_device_handle *dh)
{
	ch340g_set_scl(dh, 1);
	ch340g_set_sda(dh, 1);
	usleep(CH341_I2C_DELAY_US);
}

static void ch340g_i2c_start(libusb_device_handle *dh)
{
	ch340g_set_sda(dh, 0);
	ch340g_set_scl(dh, 0);
	usleep(CH341_I2C_DELAY_US);
}

static void ch340g_i2c_stop(libusb_device_handle *dh)
{
	ch340g_set_sda(dh, 0);
	ch340g_set_scl(dh, 1);
	ch340g_set_sda(dh, 1);
	usleep(CH341_I2C_DELAY_US);
}

static bool ch340g_tx_byte(libusb_device_handle *dh, const uint8_t byte)
{
	uint8_t i;
	for(i=0; i<8; i++)
	{
		if(byte&(1<<(7-i)))
			ch340g_set_sda(dh, 1);
		else
			ch340g_set_sda(dh, 0);
		ch340g_set_scl(dh, 1);
		ch340g_set_scl(dh, 0);
	}
	
	ch340g_set_sda(dh, 1);
	ch340g_set_scl(dh, 1);
	bool ack=!ch340g_get_sda(dh);
	ch340g_set_scl(dh, 0);
	
	return ack;
}

static void ch340g_rx_byte(libusb_device_handle *dh, uint8_t * const byte, const bool is_last_one)
{
	uint8_t i;
	
	(*byte)=0;
	for(i=0; i<8; i++)
	{
		ch340g_set_scl(dh, 1);
		(*byte)|=ch340g_get_sda(dh)<<(7-i);
		ch340g_set_scl(dh, 0);
	}

	if(is_last_one)
		ch340g_set_sda(dh, 1);
	else
		ch340g_set_sda(dh, 0);
	ch340g_set_scl(dh, 1);
	ch340g_set_scl(dh, 0);
	ch340g_set_sda(dh, 1);
}

static uint8_t ch340g_24c16_read(libusb_device_handle *dh, const uint16_t addr, const uint16_t nb_bytes, uint8_t * const data)
{
	bool error=false;
	
	ch340g_i2c_start(dh);
	
	if(!ch340g_tx_byte(dh, 0xA0|((addr>>8)&0x0F)))
	{
		printf("no ACK from EEPROM - no EEPROM or wiring error?\n");
		error=true;
		goto end;
	}
	
	if(!ch340g_tx_byte(dh, addr&0xff))
	{
		printf("no ACK from EEPROM - no EEPROM or wiring error?\n");
		error=true;
		goto end;
	}
	
	ch340g_i2c_stop(dh);
	
	usleep(CH341_I2C_DELAY_US);
	
	ch340g_i2c_start(dh);
	
	if(!ch340g_tx_byte(dh, 0xA1|((addr>>8)&0x0F)))
	{
		printf("no ACK from EEPROM - no EEPROM or wiring error?\n");
		error=true;
		goto end;
	}
	
	uint8_t byte;
	unsigned int i;
	for(i=0; i<nb_bytes; i++)
	{
		ch340g_rx_byte(dh, &byte, ((i+1)==nb_bytes));
		data[i]=byte;
	}
	
end:
	ch340g_i2c_stop(dh);
	
	return error;
}

static bool ch340g_24c16_write(libusb_device_handle *dh, const uint16_t addr, const uint16_t nb_bytes, uint8_t const * const data)
{
	bool error=false;
	
	ch340g_i2c_start(dh);
	
	if(!ch340g_tx_byte(dh, 0xA0|((addr>>8)&0x0F)))
	{
		printf("no ACK from EEPROM - no EEPROM or wiring error?\n");
		error=true;
		goto end;
	}
	
	if(!ch340g_tx_byte(dh, addr&0xff))
	{
		printf("no ACK from EEPROM - no EEPROM or wiring error?\n");
		error=true;
		goto end;
	}
	
	unsigned int i;
	for(i=0; i<nb_bytes; i++)
	{
		if(!ch340g_tx_byte(dh, data[i]))
		{
			printf("no ACK from EEPROM - no EEPROM or wiring error?\n");
			error=true;
			goto end;
		}
	}
	
end:
	ch340g_i2c_stop(dh);
	
	usleep(10000); //wait for internal write operation
	
	return error;
}


static bool ch340g_probe_eeprom(libusb_device_handle *dh)
{
	char sig[2];
	ch340g_i2c_init(dh);
	if(ch340g_24c16_read(dh, 0, 2, (uint8_t*)sig))
	{
		printf("no EEPROM found\n");
		return false;
	}
	if(sig[0]!=CH341_EEPROM_SIG0 || sig[1]!=CH341_EEPROM_SIG1)
	{
		printf("invalid signature: %c%c\n", sig[0], sig[1]);
		return false;
	}
	
	return true;
}

static bool ch340g_read_identifier_from_eeprom(libusb_device_handle *dh, char * const identifier)
{
	memset(identifier, '\0', CH341_SZ_IDENT_EEPROM+1);
	
	if(ch340g_24c16_read(dh, 2, CH341_SZ_IDENT_EEPROM, (uint8_t * const)identifier))
	{
		printf("error reading identifier\n");
		return false;
	}
	
	return true;
}

static bool ch340g_write_identifier_to_eeprom(libusb_device_handle *dh, char const * const identifier)
{
	if(ch340g_24c16_write(dh, 0, 2, (uint8_t *)"CH"))
	{
		printf("error writing signature\n");
		return false;
	}
	
	uint8_t data[CH341_SZ_IDENT_EEPROM+1];
	
	memset(data, 0, CH341_SZ_IDENT_EEPROM+1);
	strcpy((char *)data, identifier);
	
	if(ch340g_24c16_write(dh, 2, CH341_SZ_IDENT_EEPROM, data))
	{
		printf("error writing identifier\n");
		return false;
	}
	
	return true;
}

static void print_usage_and_exit(void)
{
	printf("usage: ch340g_eeprom $command [$identifier]\n\n$command:\n\t--help: Print this text and exit\n\t--version: Print version and exit\n\t--read: read identifier\n\t--write $identifier: write identifier\n\n");
	exit(0);
}

typedef enum
{
	MODE_READ,
	MODE_WRITE
} read_write_t;

int main(int argc, char **argv)
{
	printf("This is ch340g_eeprom version 0.1 (c) 2023 by kittennbfive\n\nTHIS TOOL IS PROVIDED WITHOUT ANY WARRANTY!\n\nWARNING: Only connect a single CH340G to your PC when using this tool!\n\n");
	
	if(argc!=2 && argc!=3)
		print_usage_and_exit();
	
	read_write_t mode;
	
	if(!strcmp(argv[1], "--help"))
		print_usage_and_exit();
	else if(!strcmp(argv[1], "--version"))
		exit(0); //version already printed
	else if(!strcmp(argv[1], "--read"))
		mode=MODE_READ;
	else if(!strcmp(argv[1], "--write"))
		mode=MODE_WRITE;
	else
		errx(1, "unknown command: %s (try --help)", argv[1]);	
	
	libusb_context *ctx;
	int ret;
	
	ret=libusb_init(&ctx);
	if(ret)
		errx(1, "libusb_init failed: %s", libusb_error_name(ret));
	
	/*
	ret=libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
	if(ret)
		errx(1, "libusb_set_option failed: %s", libusb_error_name(ret));
	*/
	
	libusb_device_handle *dh;
	dh=libusb_open_device_with_vid_pid(ctx, 0x1a86, 0x7523); //has limitations, see https://libusb.sourceforge.io/api-1.0/group__libusb__dev.html#ga10d67e6f1e32d17c33d93ae04617392e
	if(dh==NULL)
		err(1, "libusb_open_device failed (Are you root?)");

	char identifier[CH341_SZ_IDENT_EEPROM+1];
	
	if(mode==MODE_READ)
	{
		printf("reading identifier...\n");
		
		if(ch340g_probe_eeprom(dh))
		{
			printf("EEPROM with correct signature found\n");
			if(ch340g_read_identifier_from_eeprom(dh, identifier))
			{
				printf("identifier found: \"%s\"\n", identifier);
			}
		}
	}
	else
	{
		if(argc!=3)
			errx(1, "missing identifier or too many arguments");
		
		strncpy(identifier, argv[2], CH341_SZ_IDENT_EEPROM);
		identifier[CH341_SZ_IDENT_EEPROM]='\0';
		
		printf("writing identifier \"%s\"...\n", identifier);
		
		if(!ch340g_write_identifier_to_eeprom(dh, identifier))
			printf("write to EEPROM failed\n");
		else
			printf("write successful\n");
	}
				
	libusb_close(dh);
	libusb_exit(ctx);
	
	printf("\nall done. Bye.\n\n");
	
	return 0;
}
