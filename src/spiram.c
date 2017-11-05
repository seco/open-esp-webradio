#include "spiram.h"
#include "esp8266.h"
#include "espressif/esp_common.h"

#include "FreeRTOS.h"
#include "task.h"

#define SPI_NUM 			0
#define HSPI_NUM			1

#ifndef SPIRAM_QIO_HACK
static void spi_byte_write(unsigned int spi_no, uint32_t data) IRAM;
static void spi_byte_write(unsigned int spi_no, uint32_t data)
{
	// handle invalid input number
	if(spi_no > 1)
		return;

	while(READ_PERI_REG(SPI_CMD(spi_no)) & SPI_USR);
	SET_PERI_REG_MASK(SPI_USER(spi_no), SPI_USR_COMMAND);
	CLEAR_PERI_REG_MASK(SPI_USER(spi_no), SPI_USR_MOSI|SPI_USR_MISO|SPI_USR_DUMMY|SPI_USR_ADDR);

	//SPI_FLASH_USER2 bit28-31 is cmd length,cmd bit length is value(0-15)+1,
	// bit15-0 is cmd value.
	WRITE_PERI_REG(SPI_USER2(spi_no), ((SPI_USR_COMMAND_BITLEN & 0x7) << SPI_USR_COMMAND_BITLEN_S) | data);
	SET_PERI_REG_MASK(SPI_CMD(spi_no), SPI_USR);
	while(READ_PERI_REG(SPI_CMD(spi_no)) & SPI_USR);
}
#endif

#ifdef SPIRAM_QIO_HACK
static void spiram_setup_eqio() IRAM;
static void spiram_setup_eqio()
{
	const char eqio = 0x38; // ENTER SQI MODE (EQIO) FROM SPI_NUM MODE

	taskENTER_CRITICAL();

	GPIO.CONF[0] &= ~GPIO_CONF_OPEN_DRAIN; // CS
	GPIO.ENABLE_OUT_SET = BIT(0);
	iomux_set_gpio_function(0, true);

	GPIO.CONF[6] &= ~GPIO_CONF_OPEN_DRAIN; // CLK
	GPIO.ENABLE_OUT_SET = BIT(6);
	iomux_set_gpio_function(6, true);

	GPIO.CONF[7] &= ~GPIO_CONF_OPEN_DRAIN; // MOSI
	GPIO.ENABLE_OUT_SET = BIT(7);
	iomux_set_gpio_function(7, true);

	gpio_write(0, true); // CS high
	gpio_write(6, false); // CLK low
	gpio_write(0, false); // CS low -> select 23LC1024

	for(int i=7; i>=0; --i)
	{
		if(eqio & (1<<i))
			gpio_write(7, true); // bit=1 -> MOSI high
		else
			gpio_write(7, false); // bit=0 -> MOSI low

		gpio_write(6, true); // CLK high
		gpio_write(6, false); // CLK low
	}
	gpio_write(0, true); // CS high -> deselect 23LC1024

	PIN_FUNC_SELECT(PERIPHS_IO_MUX_SD_CLK_U, FUNC_SPICLK); // CLK
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_SD_DATA0_U, FUNC_SPIQ_MISO); // MOSI

	taskEXIT_CRITICAL();
}
#endif

//Initialize the SPI_NUM port to talk to the chip.
void spiram_init()
{
	char dummy[128];

	taskENTER_CRITICAL();

#ifdef SPIRAM_QIO_HACK
	// Since MISO and MOSI are swapped, we can't use a HW SPI for switching the SPI RAM to
	// EQIO mode. That's why we need to bitbang.
	spiram_setup_eqio();
#endif

	//hspi overlap to spi, two spi masters on cspi
	SET_PERI_REG_MASK(HOST_INF_SEL, PERI_IO_CSPI_OVERLAP);

	//set higher priority for spi than hspi
	SET_PERI_REG_MASK(SPI_EXT3(SPI_NUM), 0x1);
	SET_PERI_REG_MASK(SPI_EXT3(HSPI_NUM), 0x3);
	SET_PERI_REG_MASK(SPI_USER(HSPI_NUM), SPI_CS_SETUP);

	//select HSPI_NUM CS2 ,disable HSPI_NUM CS0 and CS1
	CLEAR_PERI_REG_MASK(SPI_PIN(HSPI_NUM), SPI_CS2_DIS);
	SET_PERI_REG_MASK(SPI_PIN(HSPI_NUM), SPI_CS0_DIS |SPI_CS1_DIS);

	//SET IO MUX FOR GPIO0 , SELECT PIN FUNC AS SPI_NUM CS2
	//IT WORK AS HSPI_NUM CS2 AFTER OVERLAP(THERE IS NO PIN OUT FOR NATIVE HSPI_NUM CS1/2)
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_SPICS2);

	// SPI_NUM CLK = 80 MHz / 4 = 20 MHz
	WRITE_PERI_REG(SPI_CLOCK(HSPI_NUM), (3 << SPI_CLKCNT_N_S) | (1 << SPI_CLKCNT_H_S) | (3 << SPI_CLKCNT_L_S));

#ifdef SPIRAM_QIO
	SET_PERI_REG_MASK(SPI_USER(HSPI_NUM), SPI_CS_SETUP|SPI_CS_HOLD|SPI_USR_COMMAND);
	CLEAR_PERI_REG_MASK(SPI_USER(HSPI_NUM), SPI_FLASH_MODE);

#ifndef SPIRAM_QIO_HACK
	spi_byte_write(HSPI_NUM, 0x38);
#endif

	SET_PERI_REG_MASK(SPI_CTRL(HSPI_NUM), SPI_QIO_MODE|SPI_FASTRD_MODE);
	SET_PERI_REG_MASK(SPI_USER(HSPI_NUM), SPI_FWRITE_QIO);
#endif

	taskEXIT_CRITICAL();

	//Dummy read to clear any weird state the SPI_NUM ram chip may be in
	spiram_read(0x0, dummy, sizeof(dummy));
}

//Macro to quickly access the W-registers of the SPI peripherial
#define SPI_W(i, j) (SPI_W0(i) + ((j)*4))

//Read bytes from a memory location. The max amount of bytes that can be read is 64.
size_t spiram_read(uint32_t addr, void *buf, size_t len)
{
	uint8_t *byte_buf = buf;
	if (len > 64)
		len = 64;

	taskENTER_CRITICAL();

	while(READ_PERI_REG(SPI_CMD(HSPI_NUM))&SPI_USR) ;
#ifndef SPIRAM_QIO
	SET_PERI_REG_MASK(SPI_USER(HSPI_NUM), SPI_CS_SETUP|SPI_CS_HOLD|SPI_USR_COMMAND|SPI_USR_ADDR|SPI_USR_MISO);
	CLEAR_PERI_REG_MASK(SPI_USER(HSPI_NUM), SPI_FLASH_MODE|SPI_USR_MOSI);
	WRITE_PERI_REG(SPI_USER1(HSPI_NUM), ((0&SPI_USR_MOSI_BITLEN)<<SPI_USR_MOSI_BITLEN_S)| //no data out
			((((8*len)-1)&SPI_USR_MISO_BITLEN)<<SPI_USR_MISO_BITLEN_S)| //len bits of data in
			((23&SPI_USR_ADDR_BITLEN)<<SPI_USR_ADDR_BITLEN_S)); //address is 24 bits A0-A23
	WRITE_PERI_REG(SPI_ADDR(HSPI_NUM), addr<<8); //write address
	WRITE_PERI_REG(SPI_USER2(HSPI_NUM), (((7&SPI_USR_COMMAND_BITLEN)<<SPI_USR_COMMAND_BITLEN_S) | 0x03));
#else
	SET_PERI_REG_MASK(SPI_USER(HSPI_NUM), SPI_USR_ADDR|SPI_USR_MISO|SPI_USR_DUMMY);
	CLEAR_PERI_REG_MASK(SPI_USER(HSPI_NUM), SPI_FLASH_MODE|SPI_USR_MOSI|SPI_USR_COMMAND);
	WRITE_PERI_REG(SPI_USER1(HSPI_NUM), ((0&SPI_USR_MOSI_BITLEN)<<SPI_USR_MOSI_BITLEN_S)| //no data out
			((((8*len)-1)&SPI_USR_MISO_BITLEN)<<SPI_USR_MISO_BITLEN_S)| //len bits of data in
			((31&SPI_USR_ADDR_BITLEN)<<SPI_USR_ADDR_BITLEN_S)|				//8bits command+address is 24 bits A0-A23
			((1&SPI_USR_DUMMY_CYCLELEN)<<SPI_USR_DUMMY_CYCLELEN_S)); 		//8bits dummy cycle

	addr |= 0x03000000; // prepend command

#ifdef SPIRAM_QIO_HACK
	addr = ( (addr & 0xaaaaaaaa) >> 1 ) | ( (addr & 0x55555555) << 1 ); // swap SIO0/SIO1; SIO2/SIO3
#endif

	WRITE_PERI_REG(SPI_ADDR(HSPI_NUM), addr); // command & write address
#endif

	SET_PERI_REG_MASK(SPI_CMD(HSPI_NUM), SPI_USR);
	while(READ_PERI_REG(SPI_CMD(HSPI_NUM))&SPI_USR) ;
	//Unaligned dest address. Copy 8bit at a time
	for (size_t i=0; i < len;)
	{
		uint32_t d = READ_PERI_REG(SPI_W(HSPI_NUM, i/4));
		byte_buf[i++] = d & 0xff;
		if (i < len)
			byte_buf[i++] = (d>>8) & 0xff;
		if (i < len)
			byte_buf[i++] = (d>>16) & 0xff;
		if (i < len)
			byte_buf[i++] = (d>>24) & 0xff;
	}

	taskEXIT_CRITICAL();

	return len;
}

//Write bytes to a memory location. The max amount of bytes that can be written is 64.
size_t spiram_write(uint32_t addr, const void *buf, size_t len)
{
	const uint8_t *byte_buf = buf;
	if(len > 64)
		len = 64;

	taskENTER_CRITICAL();

	while(READ_PERI_REG(SPI_CMD(HSPI_NUM)) & SPI_USR);

#ifndef SPIRAM_QIO
	SET_PERI_REG_MASK(SPI_USER(HSPI_NUM), SPI_CS_SETUP|SPI_CS_HOLD|SPI_USR_COMMAND|SPI_USR_ADDR|SPI_USR_MOSI);
	CLEAR_PERI_REG_MASK(SPI_USER(HSPI_NUM), SPI_FLASH_MODE|SPI_USR_MISO);
	WRITE_PERI_REG(SPI_USER1(HSPI_NUM), ((((8*len)-1)&SPI_USR_MOSI_BITLEN)<<SPI_USR_MOSI_BITLEN_S)| //len bitsbits of data out
			((0&SPI_USR_MISO_BITLEN)<<SPI_USR_MISO_BITLEN_S)| //no data in
			((23&SPI_USR_ADDR_BITLEN)<<SPI_USR_ADDR_BITLEN_S)); //address is 24 bits A0-A23
	WRITE_PERI_REG(SPI_ADDR(HSPI_NUM), addr<<8); //write address
	WRITE_PERI_REG(SPI_USER2(HSPI_NUM), (((7&SPI_USR_COMMAND_BITLEN)<<SPI_USR_COMMAND_BITLEN_S) | 0x02));
#else
	// No command phase; the command is the top-most byte of the address
	// Address phase is 32 bits long
	// No dummy phase in the write operation
	// No read-data phase, but a write-data phase

	SET_PERI_REG_MASK(SPI_USER(HSPI_NUM), SPI_USR_ADDR|SPI_USR_MOSI);
	CLEAR_PERI_REG_MASK(SPI_USER(HSPI_NUM), SPI_FLASH_MODE|SPI_USR_MISO|SPI_USR_COMMAND|SPI_USR_DUMMY);
	WRITE_PERI_REG(SPI_USER1(HSPI_NUM), ((((8*len)-1)&SPI_USR_MOSI_BITLEN)<<SPI_USR_MOSI_BITLEN_S)| //len bitsbits of data out
			((0&SPI_USR_MISO_BITLEN)<<SPI_USR_MISO_BITLEN_S)| //no data i
			((31&SPI_USR_ADDR_BITLEN)<<SPI_USR_ADDR_BITLEN_S)); //8bits command+address is 24 bits A0-A23

	addr |= 0x02000000; // prepend command

#ifdef SPIRAM_QIO_HACK
	addr = ( (addr & 0xaaaaaaaa) >> 1 ) | ( (addr & 0x55555555) << 1 ); // swap SIO0/SIO1; SIO2/SIO3
#endif

	WRITE_PERI_REG(SPI_ADDR(HSPI_NUM), addr); // write command & address
#endif

	//Assume unaligned src: Copy byte-wise.
	for (size_t i=0; i < len;) {
		uint32_t d = byte_buf[i++];
		if (i < len)
			d |= ((uint32_t) byte_buf[i++]) << 8;
		if (i < len)
			d |= ((uint32_t) byte_buf[i++]) << 16;
		if (i < len)
			d |= ((uint32_t) byte_buf[i++]) << 24;
		WRITE_PERI_REG(SPI_W(HSPI_NUM, (i - 1) / 4), d);
	}
	SET_PERI_REG_MASK(SPI_CMD(HSPI_NUM), SPI_USR);

	taskEXIT_CRITICAL();

	return len;
}

//Simple routine to see if the SPI_NUM actually stores bytes. This is not a full memory test, but will tell
//you if the RAM chip is connected well.
int spiram_test()
{
	int x;
	int err=0;
	char a[64];
	char b[64];
	char aa, bb;
	for (x=0; x<64; x++)
	{
		a[x]=x^(x<<2);
		b[x]=0xaa^x;
	}
	spiram_write(0x0, a, 64);
	spiram_write(0x100, b, 64);

	spiram_read(0x0, a, 64);
	spiram_read(0x100, b, 64);
	for (x=0; x<64; x++)
	{
		aa=x^(x<<2);
		bb=0xaa^x;
		if (aa!=a[x])
		{
			err=1;
			printf("aa: 0x%x != 0x%x\n", aa, a[x]);
		}
		if (bb!=b[x])
		{
			err=1;
			printf("bb: 0x%x != 0x%x\n", bb, b[x]);
		}
	}

	char buf[2] = {0x55, 0xaa};
	spiram_write(0x1, buf, 1);
	spiram_write(0x2, buf, 2);
	spiram_read(0x1, buf+1, 1);
	if(buf[0] != buf[1])
	{
		err=1;
		printf("0x%x != 0x%x\n", buf[0], buf[1]);
	}

	return err;
}
