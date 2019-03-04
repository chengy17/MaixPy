/*
 * This file is part of the OpenMV project.
 * Copyright (c) 2013/2014 Ibrahim Abdelkader <i.abdalkader@gmail.com>
 * This work is licensed under the MIT license, see the file LICENSE for details.
 *
 * Sensor abstraction layer.
 *
 */
#include <stdlib.h>
#include <string.h>
#include "mp.h"
#include "cambus.h"
#include "ov2640.h"
#include "sensor.h"
#include "framebuffer.h"
#include "omv_boardconfig.h"
#include "dvp.h"
#include "mphalport.h"
#include "plic.h"
#include "fpioa.h"
#include "syslog.h"
#include "ff.h"
extern volatile dvp_t* const dvp;

#define OV_CHIP_ID      (0x0A)
#define ON_CHIP_ID      (0x00)
#define MAX_XFER_SIZE   (0xFFFC*4)
#define systick_sleep mp_hal_delay_ms

sensor_t  sensor     = {0};
volatile static uint8_t g_dvp_finish_flag = 0;


static volatile int line = 0;
uint8_t _line_buf;
static uint8_t *dest_fb = NULL;

const int resolution[][2] = {
    {0,    0   },
    // C/SIF Resolutions
    {88,   72  },    /* QQCIF     */
    {176,  144 },    /* QCIF      */
    {352,  288 },    /* CIF       */
    {88,   60  },    /* QQSIF     */
    {176,  120 },    /* QSIF      */
    {352,  240 },    /* SIF       */
    // VGA Resolutions
    {40,   30  },    /* QQQQVGA   */
    {80,   60  },    /* QQQVGA    */
    {160,  120 },    /* QQVGA     */
    {320,  240 },    /* QVGA      */
    {640,  480 },    /* VGA       */
    {60,   40  },    /* HQQQVGA   */
    {120,  80  },    /* HQQVGA    */
    {240,  160 },    /* HQVGA     */
    // FFT Resolutions
    {64,   32  },    /* 64x32     */
    {64,   64  },    /* 64x64     */
    {128,  64  },    /* 128x64    */
    {128,  128 },    /* 128x64    */
    // Other
    {128,  160 },    /* LCD       */
    {128,  160 },    /* QQVGA2    */
    {720,  480 },    /* WVGA      */
    {752,  480 },    /* WVGA2     */
    {800,  600 },    /* SVGA      */
    {1280, 1024},    /* SXGA      */
    {1600, 1200},    /* UXGA      */
};

void _ndelay(uint32_t ns)
{
    uint32_t i;

    while (ns && ns--)
    {
        for (i = 0; i < 25; i++)
            __asm__ __volatile__("nop");
    }
}

static int sensor_irq(void *ctx)
{
	sensor_t *sensor = ctx;
	if (dvp_get_interrupt(DVP_STS_FRAME_FINISH)) {	//frame end
		dvp_clear_interrupt(DVP_STS_FRAME_FINISH);
		g_dvp_finish_flag = 1;
	} else {	//frame start
        if(g_dvp_finish_flag == 0)  //only we finish the convert, do transmit again
            dvp_start_convert();	//so we need deal img ontime, or skip one framebefore next
		dvp_clear_interrupt(DVP_STS_FRAME_START);
	}

	return 0;
}

 
extern uint8_t* g_ai_buf_in;
// extern uint8_t* g_ai_buf_out;
extern uint8_t* g_dvp_buf;
// extern uint8_t* g_lcd_buf;
extern uint8_t* g_jpg_buf;

void sensor_init_fb()
{
    // Init FB mutex
    //TODO:
    // mutex_init(&JPEG_FB()->lock);

    // Save fb_enabled flag state
    int fb_enabled = JPEG_FB()->enabled;

    // Clear framebuffers
	MAIN_FB()->x=0;MAIN_FB()->y=0;
	MAIN_FB()->w=0;MAIN_FB()->h=0;
	MAIN_FB()->u=0;MAIN_FB()->v=0;
	MAIN_FB()->bpp=0;
	MAIN_FB()->pixels = &g_dvp_buf;
	MAIN_FB()->pix_ai = &g_ai_buf_in;
	JPEG_FB()->w=0;JPEG_FB()->h=0;
	JPEG_FB()->size=0;JPEG_FB()->enabled=0;
	JPEG_FB()->quality=0;
	JPEG_FB()->pixels = &g_jpg_buf;
	//printf("pixels=0x%x, pix_ai=0x%x, jpg=0x%x\n", MAIN_FB()->pixels, MAIN_FB()->pix_ai, JPEG_FB()->pixels);
    // Set default quality
    JPEG_FB()->quality = 35;

    // Set fb_enabled
    JPEG_FB()->enabled = fb_enabled;
}

//-------------------------------Monocular--------------------------------------

int sensor_init_dvp()
{
    int init_ret = 0;
	
	fpioa_set_function(47, FUNC_CMOS_PCLK);
	fpioa_set_function(46, FUNC_CMOS_XCLK);
	fpioa_set_function(45, FUNC_CMOS_HREF);
	fpioa_set_function(44, FUNC_CMOS_PWDN);
	fpioa_set_function(43, FUNC_CMOS_VSYNC);
	fpioa_set_function(42, FUNC_CMOS_RST);
	fpioa_set_function(41, FUNC_SCCB_SCLK);
	fpioa_set_function(40, FUNC_SCCB_SDA);

    /* Do a power cycle */
    DCMI_PWDN_HIGH();
    mp_hal_ticks_ms(10);

    DCMI_PWDN_LOW();
    mp_hal_ticks_ms(10);

    // Initialize the camera bus, 8bit reg
    cambus_init(8);
	 // Initialize dvp interface
	dvp_set_xclk_rate(24000000);
	 dvp->cmos_cfg |= DVP_CMOS_CLK_DIV(3) | DVP_CMOS_CLK_ENABLE;
	dvp_enable_burst();
	dvp_disable_auto();
	dvp_set_output_enable(0, 1);	//enable to AI
	dvp_set_output_enable(1, 1);	//enable to lcd
	dvp_set_image_format(DVP_CFG_RGB_FORMAT);
	dvp_set_image_size(OMV_INIT_W, OMV_INIT_H);	//set QVGA default
	dvp_set_ai_addr(MAIN_FB()->pix_ai, (uint32_t)(MAIN_FB()->pix_ai + OMV_INIT_W * OMV_INIT_H), (uint32_t)(MAIN_FB()->pix_ai + OMV_INIT_W * OMV_INIT_H * 2));
	dvp_set_display_addr(MAIN_FB()->pixels);
    /* Some sensors have different reset polarities, and we can't know which sensor
       is connected before initializing cambus and probing the sensor, which in turn
       requires pulling the sensor out of the reset state. So we try to probe the
       sensor with both polarities to determine line state. */
    sensor.pwdn_pol = ACTIVE_HIGH;
    sensor.reset_pol = ACTIVE_HIGH;

    /* Reset the sensor */
    DCMI_RESET_HIGH();
    mp_hal_ticks_ms(10);

    DCMI_RESET_LOW();
    mp_hal_ticks_ms(10);

    /* Probe the sensor */
    sensor.slv_addr = cambus_scan();
    if (sensor.slv_addr == 0) {
        /* Sensor has been held in reset,
           so the reset line is active low */
        sensor.reset_pol = ACTIVE_LOW;

        /* Pull the sensor out of the reset state,systick_sleep() */
        DCMI_RESET_HIGH();
        mp_hal_delay_ms(10);

        /* Probe again to set the slave addr */
        sensor.slv_addr = cambus_scan();
        if (sensor.slv_addr == 0) {
            sensor.pwdn_pol = ACTIVE_LOW;

            DCMI_PWDN_HIGH();
            mp_hal_delay_ms(10);

            sensor.slv_addr = cambus_scan();
            if (sensor.slv_addr == 0) {
                sensor.reset_pol = ACTIVE_HIGH;

                DCMI_RESET_LOW();
                mp_hal_delay_ms(10);

                sensor.slv_addr = cambus_scan();
                if (sensor.slv_addr == 0) {
                    return -2;
                }
            }
        }
    }

    // Clear sensor chip ID.
    sensor.chip_id = 0;

    // Set default snapshot function.
    sensor.snapshot = sensor_snapshot;
	sensor.flush = sensor_flush;
    if (sensor.slv_addr == LEPTON_ID) {
        sensor.chip_id = LEPTON_ID;
		/*set LEPTON xclk rate*/
		/*lepton_init*/
    } else {
        // Read ON semi sensor ID.
        cambus_readb(sensor.slv_addr, ON_CHIP_ID, &sensor.chip_id);
        if (sensor.chip_id == MT9V034_ID) {
			/*set MT9V034 xclk rate*/
			/*mt9v034_init*/
        } else { // Read OV sensor ID.
            cambus_readb(sensor.slv_addr, OV_CHIP_ID, &sensor.chip_id);
            // Initialize sensor struct.
            switch (sensor.chip_id) {
                case OV9650_ID:
					/*ov9650_init*/
                    break;
                case OV2640_ID:
                    init_ret = ov2640_init(&sensor);
                    break;
                case OV7725_ID:
					/*ov7725_init*/
                    break;
                default:
                    // Sensor is not supported.
                    return -3;
            }
        }
    }


    if (init_ret != 0 ) {
        // Sensor init failed.
        return -4;
    }
	
    // Clear fb_enabled flag
    // This is executed only once to initialize the FB enabled flag.
    JPEG_FB()->enabled = 0;

    /* All good! */
	printf("[MAIXPY]: exit sensor_init\n");
    return 0;
}
int sensor_init_irq()
{
	dvp_config_interrupt(DVP_CFG_START_INT_ENABLE | DVP_CFG_FINISH_INT_ENABLE, 0);
	plic_set_priority(IRQN_DVP_INTERRUPT, 2);
    /* set irq handle */
	plic_irq_register(IRQN_DVP_INTERRUPT, sensor_irq, (void*)&sensor);
	
	plic_irq_disable(IRQN_DVP_INTERRUPT);
	dvp_clear_interrupt(DVP_STS_FRAME_START | DVP_STS_FRAME_FINISH);
	dvp_config_interrupt(DVP_CFG_START_INT_ENABLE | DVP_CFG_FINISH_INT_ENABLE, 1);
	
	return 0;
}

int sensor_reset()
{
	sensor_init_fb();		//init FB
	sensor_init_dvp();		//init pins, scan I2C, do ov2640 init
    // Reset the sesnor state
    sensor.sde         = 0;
    sensor.pixformat   = 0;
    sensor.framesize   = 0;
    sensor.framerate   = 0;
    sensor.gainceiling = 0;
    if(sensor.reset == NULL)
    {
        printf("[MAIXPY]: sensor reset function is null\n");
        return -1;
    }
    // Call sensor-specific reset function
    if (sensor.reset(&sensor) != 0) {	//rst reg, set default cfg.
        return -1;
    }
    // Disable dvp  IRQ before all cfg done 
    sensor_init_irq();

	printf("[MAIXPY]: exit sensor_reset\n");
    return 0;
}

//-------------------------------Binocular--------------------------------------

int binocular_sensor_init_dvp()
{
	fpioa_set_function(47, FUNC_CMOS_PCLK);
	fpioa_set_function(46, FUNC_CMOS_XCLK);
	fpioa_set_function(45, FUNC_CMOS_HREF);
	fpioa_set_function(44, FUNC_CMOS_PWDN);
	fpioa_set_function(43, FUNC_CMOS_VSYNC);
	fpioa_set_function(42, FUNC_CMOS_RST);
	fpioa_set_function(41, FUNC_SCCB_SCLK);
	fpioa_set_function(40, FUNC_SCCB_SDA);

    /* Do a power cycle */
    DCMI_PWDN_HIGH();
    mp_hal_ticks_ms(10);

    DCMI_PWDN_LOW();
    mp_hal_ticks_ms(10);

    // Initialize the camera bus, 8bit reg
    cambus_init(8);
	 // Initialize dvp interface
	dvp_set_xclk_rate(24000000);
	dvp->cmos_cfg |= DVP_CMOS_CLK_DIV(3) | DVP_CMOS_CLK_ENABLE;
	dvp_enable_burst();
	dvp_disable_auto();
	dvp_set_output_enable(0, 1);	//enable to AI
	dvp_set_output_enable(1, 1);	//enable to lcd
	dvp_set_image_format(DVP_CFG_RGB_FORMAT);
	dvp_set_image_size(OMV_INIT_W, OMV_INIT_H);	//set QVGA default
	dvp_set_ai_addr(MAIN_FB()->pix_ai, (uint32_t)(MAIN_FB()->pix_ai + OMV_INIT_W * OMV_INIT_H), (uint32_t)(MAIN_FB()->pix_ai + OMV_INIT_W * OMV_INIT_H * 2));
	dvp_set_display_addr(MAIN_FB()->pixels);
    /* Some sensors have different reset polarities, and we can't know which sensor
       is connected before initializing cambus and probing the sensor, which in turn
       requires pulling the sensor out of the reset state. So we try to probe the
       sensor with both polarities to determine line state. */
    sensor.pwdn_pol = ACTIVE_BINOCULAR;
    sensor.reset_pol = ACTIVE_HIGH;

}

int binocular_sensor_scan()
{
    int init_ret = 0;
    //reset both sensor
    DCMI_PWDN_HIGH();
    mp_hal_ticks_ms(10);
    DCMI_RESET_LOW();
    mp_hal_ticks_ms(10);
    DCMI_RESET_HIGH();
    mp_hal_ticks_ms(10);

    DCMI_PWDN_LOW();
    mp_hal_ticks_ms(10);
    DCMI_RESET_LOW();
    mp_hal_ticks_ms(10);
    DCMI_RESET_HIGH();
    mp_hal_ticks_ms(10);

    /* Probe the first sensor */
    DCMI_PWDN_HIGH();
    mp_hal_ticks_ms(10);   
    sensor.slv_addr = cambus_scan();
    if (sensor.slv_addr == 0) {
        printf("[MAIXPY]: Can not find sensor first\n");
        /* Sensor has been held in reset,
           so the reset line is active low */
        sensor.reset_pol = ACTIVE_LOW;
        /* Pull the sensor out of the reset state,systick_sleep() */
        DCMI_RESET_HIGH();
        mp_hal_delay_ms(10);
        /* Probe again to set the slave addr */
        sensor.slv_addr = cambus_scan();
        if (sensor.slv_addr == 0) {
            printf("[MAIXPY]: Don't detect sensor\n");
            return -1;
        }
    }
            
    /* find  the second sensor */
    DCMI_PWDN_LOW();
    mp_hal_ticks_ms(10);
    if(sensor.slv_addr != cambus_scan())
    {
        printf("[MAIXPY]: sensors don't match\n");
        return -2;
    }
    // Clear sensor chip ID.
    sensor.chip_id = 0;

    // Set default snapshot function.
    sensor.snapshot = sensor_snapshot;
	sensor.flush = sensor_flush;
    if (sensor.slv_addr == LEPTON_ID) {
        sensor.chip_id = LEPTON_ID;
		/*set LEPTON xclk rate*/
		/*lepton_init*/
    } else {
        // Read ON semi sensor ID.
        cambus_readb(sensor.slv_addr, ON_CHIP_ID, &sensor.chip_id);
        if (sensor.chip_id == MT9V034_ID) {
			/*set MT9V034 xclk rate*/
			/*mt9v034_init*/
        } else { // Read OV sensor ID.
            cambus_readb(sensor.slv_addr, OV_CHIP_ID, &sensor.chip_id);
            // Initialize sensor struct.
            switch (sensor.chip_id) {
                case OV9650_ID:
					/*ov9650_init*/
                    break;
                case OV2640_ID:
                    printf("[MAIXPY]: ov2640_init\n");
                    init_ret = ov2640_init(&sensor);
                    break;
                case OV7725_ID:
					/*ov7725_init*/
                    break;
                default:
                    // Sensor is not supported.
                    return -3;
            }
        }
    }

    if (init_ret != 0 ) {
        // Sensor init failed.
        return -4;
    }
    // Clear fb_enabled flag
    // This is executed only once to initialize the FB enabled flag.
    JPEG_FB()->enabled = 0;

    /* All good! */
	printf("[MAIXPY]: exit sensor_init\n");
    return 0;
}

int binocular_sensor_reset()
{
	sensor_init_fb();		//init FB
	binocular_sensor_init_dvp();//init pins and dvp interface
    if(0 != binocular_sensor_scan())//scan I2C, do ov2640 init
    {
        printf("[MAIXPY]: scan sensor error\n");
        return -1;
    }
    // Reset the sesnor state
    sensor.sde         = 0;
    sensor.pixformat   = 0;
    sensor.framesize   = 0;
    sensor.framerate   = 0;
    sensor.gainceiling = 0;

    //select first sensor ,  Call sensor-specific reset function
    DCMI_PWDN_HIGH();
    mp_hal_ticks_ms(10);
    DCMI_RESET_LOW();
    mp_hal_ticks_ms(10);
    DCMI_RESET_HIGH();
    mp_hal_ticks_ms(10); 

    if (sensor.reset(&sensor) != 0) {	//rst reg, set default cfg.
        printf("[MAIXPY]: First sensor reset failed\n");
        return -1;
    }

    //select second sensor ,  Call sensor-specific reset function
    DCMI_PWDN_LOW();
    mp_hal_ticks_ms(10);
    DCMI_RESET_LOW();
    mp_hal_ticks_ms(10);
    DCMI_RESET_HIGH();
    mp_hal_ticks_ms(10);

    if (sensor.reset(&sensor) != 0) {	//rst reg, set default cfg.
        printf("[MAIXPY]: Second sensor reset failed\n");
        return -1;
    }

    // Disable dvp  IRQ before all cfg done 
    sensor_init_irq();

	printf("[MAIXPY]: exit sensor_reset\n");
    return 0;
}


int sensor_get_id()
{
    return sensor.chip_id;
}

int sensor_sleep(int enable)
{
    if (sensor.sleep == NULL
        || sensor.sleep(&sensor, enable) != 0) {
        // Operation not supported
        return -1;
    }
    return 0;
}


int sensor_shutdown(int enable)
{
    if (enable) {
        DCMI_PWDN_HIGH();
    } else {
        DCMI_PWDN_LOW();
    }

    systick_sleep(10);
    return 0;
}


int sensor_read_reg(uint8_t reg_addr)
{
    if (sensor.read_reg == NULL) {
        // Operation not supported
        return -1;
    }
    return sensor.read_reg(&sensor, reg_addr);
}

int sensor_write_reg(uint8_t reg_addr, uint16_t reg_data)
{
    if (sensor.write_reg == NULL) {
        // Operation not supported
        return -1;
    }
    return sensor.write_reg(&sensor, reg_addr, reg_data);
}

int sensor_set_pixformat(pixformat_t pixformat)
{

    if (sensor.set_pixformat == NULL
        || sensor.set_pixformat(&sensor, pixformat) != 0) {
        // Operation not supported
        return -1;
    }
    // Set pixel format
    sensor.pixformat = pixformat;
    // Skip the first frame.
    MAIN_FB()->bpp = -1;

    return 0;
}

int sensor_set_framesize(framesize_t framesize)
{
    // Call the sensor specific function
    if (sensor.set_framesize == NULL
        || sensor.set_framesize(&sensor, framesize) != 0) {
        // Operation not supported
        return -1;
    }
    // Set framebuffer size
    sensor.framesize = framesize;
    // Skip the first frame.
    MAIN_FB()->bpp = -1;
    // Set MAIN FB x, y offset.
    MAIN_FB()->x = 0;
    MAIN_FB()->y = 0;
    // Set MAIN FB width and height.
    MAIN_FB()->w = resolution[framesize][0];
    MAIN_FB()->h = resolution[framesize][1];

    // Set MAIN FB backup width and height.
    MAIN_FB()->u = resolution[framesize][0];
    MAIN_FB()->v = resolution[framesize][1];
    return 0;
}

int sensor_set_framerate(framerate_t framerate)
{
    if (sensor.framerate == framerate) {
       /* no change */
        return 0;
    }

    /* call the sensor specific function */
    if (sensor.set_framerate == NULL
        || sensor.set_framerate(&sensor, framerate) != 0) {
        /* operation not supported */
        return -1;
    }

    /* set the frame rate */
    sensor.framerate = framerate;

    return 0;
}

int sensor_set_windowing(int x, int y, int w, int h)
{	//TODO： set camera windows
    MAIN_FB()->x = x;
    MAIN_FB()->y = y;
    MAIN_FB()->w = MAIN_FB()->u = w;
    MAIN_FB()->h = MAIN_FB()->v = h;
    return 0;
}

int sensor_set_contrast(int level)
{
    if (sensor.set_contrast != NULL) {
        return sensor.set_contrast(&sensor, level);
    }
    return -1;
}

int sensor_set_brightness(int level)
{
    if (sensor.set_brightness != NULL) {
        return sensor.set_brightness(&sensor, level);
    }
    return -1;
}

int sensor_set_saturation(int level)
{
    if (sensor.set_saturation != NULL) {
        return sensor.set_saturation(&sensor, level);
    }
    return -1;
}

int sensor_set_gainceiling(gainceiling_t gainceiling)
{
    if (sensor.gainceiling == gainceiling) {
        /* no change */
        return 0;
    }

    /* call the sensor specific function */
    if (sensor.set_gainceiling == NULL
        || sensor.set_gainceiling(&sensor, gainceiling) != 0) {
        /* operation not supported */
        return -1;
    }

    sensor.gainceiling = gainceiling;
    return 0;
}

int sensor_set_quality(int qs)
{
    /* call the sensor specific function */
    if (sensor.set_quality == NULL
        || sensor.set_quality(&sensor, qs) != 0) {
        /* operation not supported */
        return -1;
    }
    return 0;
}

int sensor_set_colorbar(int enable)
{
    /* call the sensor specific function */
    if (sensor.set_colorbar == NULL
        || sensor.set_colorbar(&sensor, enable) != 0) {
        /* operation not supported */
        return -1;
    }
    return 0;
}

int sensor_set_auto_gain(int enable, float gain_db, float gain_db_ceiling)
{
    /* call the sensor specific function */
    if (sensor.set_auto_gain == NULL
        || sensor.set_auto_gain(&sensor, enable, gain_db, gain_db_ceiling) != 0) {
        /* operation not supported */
        return -1;
    }
    return 0;
}

int sensor_get_gain_db(float *gain_db)
{
    /* call the sensor specific function */
    if (sensor.get_gain_db == NULL
        || sensor.get_gain_db(&sensor, gain_db) != 0) {
        /* operation not supported */
        return -1;
    }
    return 0;
}

int sensor_set_auto_exposure(int enable, int exposure_us)
{
    /* call the sensor specific function */
    if (sensor.set_auto_exposure == NULL
        || sensor.set_auto_exposure(&sensor, enable, exposure_us) != 0) {
        /* operation not supported */
        return -1;
    }
    return 0;
}

int sensor_get_exposure_us(int *exposure_us)
{
    /* call the sensor specific function */
    if (sensor.get_exposure_us == NULL
        || sensor.get_exposure_us(&sensor, exposure_us) != 0) {
        /* operation not supported */
        return -1;
    }
    return 0;
}

int sensor_set_auto_whitebal(int enable, float r_gain_db, float g_gain_db, float b_gain_db)
{
    /* call the sensor specific function */
    if (sensor.set_auto_whitebal == NULL
        || sensor.set_auto_whitebal(&sensor, enable, r_gain_db, g_gain_db, b_gain_db) != 0) {
        /* operation not supported */
        return -1;
    }
    return 0;
}

int sensor_get_rgb_gain_db(float *r_gain_db, float *g_gain_db, float *b_gain_db)
{
    /* call the sensor specific function */
    if (sensor.get_rgb_gain_db == NULL
        || sensor.get_rgb_gain_db(&sensor, r_gain_db, g_gain_db, b_gain_db) != 0) {
        /* operation not supported */
        return -1;
    }
    return 0;
}

int sensor_set_hmirror(int enable)
{
    /* call the sensor specific function */
    if (sensor.set_hmirror == NULL
        || sensor.set_hmirror(&sensor, enable) != 0) {
        /* operation not supported */
        return -1;
    }
    return 0;
}

int sensor_set_vflip(int enable)
{
    /* call the sensor specific function */
    if (sensor.set_vflip == NULL
        || sensor.set_vflip(&sensor, enable) != 0) {
        /* operation not supported */
        return -1;
    }
    return 0;
}

int sensor_set_special_effect(sde_t sde)
{
    if (sensor.sde == sde) {
        /* no change */
        return 0;
    }

    /* call the sensor specific function */
    if (sensor.set_special_effect == NULL
        || sensor.set_special_effect(&sensor, sde) != 0) {
        /* operation not supported */
        return -1;
    }

    sensor.sde = sde;
    return 0;
}

int sensor_set_lens_correction(int enable, int radi, int coef)
{
    /* call the sensor specific function */
    if (sensor.set_lens_correction == NULL
        || sensor.set_lens_correction(&sensor, enable, radi, coef) != 0) {
        /* operation not supported */
        return -1;
    }

    return 0;
}

/*
int sensor_set_vsync_output(GPIO_TypeDef *gpio, uint32_t pin)
{
    sensor.vsync_pin  = pin;
    sensor.vsync_gpio = gpio;
    // Enable VSYNC EXTI IRQ
    NVIC_SetPriority(DCMI_VSYNC_IRQN, IRQ_PRI_EXTINT);
    HAL_NVIC_EnableIRQ(DCMI_VSYNC_IRQN);
    return 0;
}
*/

int sensor_run(int enable)
{
	if(enable)
	{
		dvp_clear_interrupt(DVP_STS_FRAME_START | DVP_STS_FRAME_FINISH);
		plic_irq_enable(IRQN_DVP_INTERRUPT);
		dvp_config_interrupt(DVP_CFG_START_INT_ENABLE | DVP_CFG_FINISH_INT_ENABLE, 1);
	}
	else{
		plic_irq_disable(IRQN_DVP_INTERRUPT);
		dvp_clear_interrupt(DVP_STS_FRAME_START | DVP_STS_FRAME_FINISH);
		dvp_config_interrupt(DVP_CFG_START_INT_ENABLE | DVP_CFG_FINISH_INT_ENABLE, 1);
	}
    return 0;
}

static void sensor_check_buffsize()
{
    int bpp=0;
    switch (sensor.pixformat) {
        case PIXFORMAT_BAYER:
        case PIXFORMAT_GRAYSCALE:
            bpp = 1;
            break;
        case PIXFORMAT_YUV422:
        case PIXFORMAT_RGB565:
            bpp = 2;
            break;
        default:
            break;
    }

    if ((MAIN_FB()->w * MAIN_FB()->h * bpp) > (OMV_INIT_W * OMV_INIT_H * OMV_INIT_BPP)) {
		printf("%s: Image size too big to fit into buf!\n", __func__);
        if (sensor.pixformat == PIXFORMAT_GRAYSCALE) {
            // Crop higher GS resolutions to QVGA
            sensor_set_windowing(190, 120, 320, 240);
        } else if (sensor.pixformat == PIXFORMAT_RGB565) {
            // Switch to BAYER if the frame is too big to fit in RAM.
            sensor_set_pixformat(PIXFORMAT_BAYER);
        }
    }

}

int exchang_data_byte(uint8_t* addr,uint32_t length)
{
  if(NULL == addr)
    return -1;
  uint8_t data = 0;
  for(int i = 0 ; i < length ;i = i + 2)
  {
    data = addr[i];
    addr[i] = addr[i + 1];
    addr[i + 1] = data;
  }
  return 0;
}
int exchang_pixel(uint16_t* addr,uint32_t resoltion)
{
  if(NULL == addr)
    return -1;
  uint16_t data = 0;
  for(int i = 0 ; i < resoltion ;i = i + 2)
  {
    data = addr[i];
    addr[i] = addr[i + 1];
    addr[i + 1] = data;
  }
  return 0;
}

int reverse_u32pixel(uint32_t* addr,uint32_t length)
{
  if(NULL == addr)
    return -1;

  uint32_t data;
  uint32_t* pend = addr+length;
  for(;addr<pend;addr++)
  {
	  data = *(addr);
	  *(addr) = ((data & 0x000000FF) << 24) | ((data & 0x0000FF00) << 8) | 
                ((data & 0x00FF0000) >> 8) | ((data & 0xFF000000) >> 24) ;
  }  //1.7ms
  
  
  return 0;
}

void sensor_flush(void)
{	//flush old frame, let dvp capture new image
	//use it when you don't snap for a while.
	g_dvp_finish_flag = 0;
	return ;
}

int sensor_snapshot(sensor_t *sensor, image_t *image, streaming_cb_t streaming_cb)
{	
    bool streaming = (streaming_cb != NULL); // Streaming mode.
	if(image == NULL) return -1;
    // Compress the framebuffer for the IDE preview, only if it's not the first frame,
    // the framebuffer is enabled and the image sensor does not support JPEG encoding.
    // Note: This doesn't run unless the IDE is connected and the framebuffer is enabled.
    //fb_update_jpeg_buffer();

    // Make sure the raw frame fits into the FB. If it doesn't it will be cropped if
    // the format is set to GS, otherwise the pixel format will be swicthed to BAYER.
    sensor_check_buffsize();
	
    // The user may have changed the MAIN_FB width or height on the last image so we need
    // to restore that here. We don't have to restore bpp because that's taken care of
    // already in the code below. Note that we do the JPEG compression above first to save
    // the FB of whatever the user set it to and now we restore.
    MAIN_FB()->w = MAIN_FB()->u;
    MAIN_FB()->h = MAIN_FB()->v;

    if (streaming_cb) {
        image->pixels = NULL;
    }

    do {
        if (streaming_cb  && image->pixels != NULL) {  //&& doublebuf
            // Call streaming callback function with previous frame.
            // Note: Image pointer should Not be NULL in streaming mode.
            streaming = streaming_cb(image);
        }
	    // Fix the BPP
        switch (sensor->pixformat) {
            case PIXFORMAT_GRAYSCALE:
                MAIN_FB()->bpp = 1;
                break;
            case PIXFORMAT_YUV422:
            case PIXFORMAT_RGB565:
                MAIN_FB()->bpp = 2;
                break;
            case PIXFORMAT_BAYER:
                MAIN_FB()->bpp = 3;
                break;
            case PIXFORMAT_JPEG:
                // Read the number of data items transferred
                MAIN_FB()->bpp = MAX_XFER_SIZE * 4;
                break;
            default:
                break;
        }
		//
		if(MAIN_FB()->bpp > 3)
		{
			printf("[MaixPy] %s | bpp error\n",__func__);
			return -1;
		}
		
		//wait for new frame
		g_dvp_finish_flag = 0;
        uint32_t start =  systick_current_millis();
		while (g_dvp_finish_flag == 0)
        {
            _ndelay(50);
            if(systick_current_millis() - start > 300)//wait for 30ms
                return -1;
        }
        // Set the user image.
		image->w = MAIN_FB()->w;
		image->h = MAIN_FB()->h;
		image->bpp = MAIN_FB()->bpp;
		image->pixels = MAIN_FB()->pixels;
		image->pix_ai = MAIN_FB()->pix_ai;
		//as data come in is in u32 LE format, we need exchange its order
		//unsigned long t0,t1;
		//t0=read_cycle();
		//exchang_data_byte((image->pixels), (MAIN_FB()->w)*(MAIN_FB()->h)*2);
		//exchang_pixel((image->pixels), (MAIN_FB()->w)*(MAIN_FB()->h)); //cost 3ms@400M
		reverse_u32pixel((image->pixels), (MAIN_FB()->w)*(MAIN_FB()->h)/2);
		//t1=read_cycle();
		//printf("%ld-%ld=%ld, %ld us!\r\n",t1,t0,(t1-t0),((t1-t0)*1000000/400000000)); 
		if (streaming_cb) {
			// In streaming mode, either switch frame buffers in double buffer mode,
			// or call the streaming callback with the main FB in single buffer mode.
				// In single buffer mode, call streaming callback.
				streaming = streaming_cb(image);
		}
    } while (streaming == true);

    return 0;
}
