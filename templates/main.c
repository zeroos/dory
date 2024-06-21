/*-----------------------------------------------------------------------------
 Copyright (C) 2020-2021 ETH Zurich, Switzerland, University of Bologna, Italy.
 All rights reserved.                                                           
                                                                               
 Licensed under the Apache License, Version 2.0 (the "License");               
 you may not use this file except in compliance with the License.              
 See LICENSE.apache.md in the top directory for details.                       
 You may obtain a copy of the License at                                       
                                                                               
   http://www.apache.org/licenses/LICENSE-2.0                                  
                                                                               
 Unless required by applicable law or agreed to in writing, software           
 distributed under the License is distributed on an "AS IS" BASIS,             
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.      
 See the License for the specific language governing permissions and           
 limitations under the License.                                                
                                                                               
 File:    main.c                                                               
 Author:  Vlad Niculescu      <vladn@iis.ee.ethz.ch>                           
 Date:    18.02.2021                                                           
-------------------------------------------------------------------------------*/
#include "network.h"

#include "pmsis.h"
#include "bsp/flash/hyperflash.h"
#include "bsp/bsp.h"
#include "bsp/buffer.h"
#include "bsp/camera/himax.h"
#include "bsp/ram.h"
#include "bsp/ram/hyperram.h"
#include "bsp/display/ili9341.h"

#include "gaplib/ImgIO.h"
#include "Gap.h"

#include "bsp/fs.h"
#include "bsp/flash/hyperflash.h"
#include "bsp/fs/readfs.h"
#include "bsp/transport/nina_w10.h"
#include "tools/frame_streamer.h"

// Verbose
#define VERBOSE 1
// #define PERF 1 // print FPS performances of the network 

// Change Mode
//#define REGRESSION_AS_CLASSIFICATION 1
//#define IMAV 1

// Defines 
#define FREQ_FC      200
#define FREQ_CL      175

// Camera
#ifdef IMAV
	#define CAMERA_WIDTH    162
	#define CAMERA_HEIGHT   162
	#define INPUT_WIDTH  162
	#define INPUT_HEIGHT 162
	#define INPUT_COLORS 1
#else
	#define CAMERA_WIDTH    324
	#define CAMERA_HEIGHT   244
	#define INPUT_WIDTH  200
	#define INPUT_HEIGHT 200
	#define INPUT_COLORS 1
#endif

#define CAMERA_SIZE   	(CAMERA_HEIGHT*CAMERA_WIDTH)
#define BUFF_SIZE       (CAMERA_WIDTH*CAMERA_HEIGHT)

//LED 
static struct pi_device gpio_device;
#define LED_ON pi_gpio_pin_write(&gpio_device, 2, 1)
#define LED_OFF pi_gpio_pin_write(&gpio_device, 2, 0)

//streaming
// #define JPEG_STREAMER 1
#define STREAM_WIDTH CAMERA_WIDTH
#define STREAM_HEIGHT CAMERA_HEIGHT

// GAP8 OUTPUT Size
#ifdef REGRESSION_AS_CLASSIFICATION
    #define CNN_OUTPUTS 4
#elif IMAV
    #define CNN_OUTPUTS 7
#else
    #define CNN_OUTPUTS 2
#endif


// Global Variables
static pi_buffer_t buffer;
struct pi_device HyperRam;
static struct pi_device camera;
int32_t data_to_send[CNN_OUTPUTS];

static struct pi_device wifi;
static int open_wifi(struct pi_device *device)
{
  struct pi_nina_w10_conf nina_conf;

  pi_nina_w10_conf_init(&nina_conf);

  nina_conf.ssid = "";
  nina_conf.passwd = "";
  nina_conf.ip_addr = "0.0.0.0";
  nina_conf.port = 5555;
  pi_open_from_conf(device, &nina_conf);
  if (pi_transport_open(device))
    return -1;

  return 0;
}

static frame_streamer_t *streamer;
static frame_streamer_t *open_streamer(char *name)
{
  struct frame_streamer_conf frame_streamer_conf;

  frame_streamer_conf_init(&frame_streamer_conf);

  frame_streamer_conf.transport = &wifi;
  frame_streamer_conf.format = FRAME_STREAMER_FORMAT_JPEG;
  frame_streamer_conf.width = STREAM_WIDTH;
  frame_streamer_conf.height = STREAM_HEIGHT;
  frame_streamer_conf.depth = 1;
  frame_streamer_conf.name = name;

  return frame_streamer_open(&frame_streamer_conf);
}


static struct pi_device camera;
static void open_camera() {

	int32_t errors = 0;
	uint8_t set_value = 3;
	struct pi_himax_conf cam_conf;

	pi_himax_conf_init(&cam_conf);

	cam_conf.format = PI_CAMERA_QVGA;

	pi_open_from_conf(&camera, &cam_conf);

	errors = pi_camera_open(&camera);

	printf("HiMax camera init:\t\t\t%s\n", errors?"Failed":"Ok");

	if(errors) pmsis_exit(errors);

	// image rotation
	pi_camera_reg_set(&camera, IMG_ORIENTATION, &set_value);
	pi_camera_control(&camera, PI_CAMERA_CMD_AEG_INIT, 0);
}



void image_crop(uint8_t* image_raw, uint8_t* image_cropped)
{
    for (uint16_t i = 0; i < 200; i++)
    {
        for (uint16_t j = 0; j < 200; j++) {
            *(image_cropped+i*INPUT_WIDTH+j) = *(image_raw+(i+44)*CAMERA_WIDTH+j+62);
        }
    }
}

// PERFORMANCES
void start_perf_counter(){
	#ifdef PERF
	// configure
	pi_perf_conf(1<<PI_PERF_CYCLES); 
	// perf measurement begin
	pi_perf_reset();                      
	pi_perf_start();		
	#endif
}

void end_perf_counter(bool verbose){

	#ifdef PERF
	// performance measurements: end
	pi_perf_stop();
	float perf_cyc =  pi_perf_read(PI_PERF_CYCLES);
	float perf_s = 1./((float)perf_cyc/(float)(FREQ_FC*1000*1000));
	// printf("%d\n", perf_cyc);
	printf("%f FPS \n", perf_s);
	#endif
}


PI_L2 unsigned char *image_in;
int32_t *ResOut;
/*  ResOut description:
	- Dronet (regression yaw)
		ResOut[0] = steering
		ResOut[1] = collision
	- Dronet (classification yaw)
		ResOut[0] = steering left
		ResOut[1] = straight
		ResOut[2] = steering right
		ResOut[3] = collision
	- Dronet (IMAV)
		ResOut[0] = Edge     visible
		ResOut[0] = Edge not visible
		ResOut[0] = Corner   visible
		ResOut[1] = Yaw
		ResOut[3] = collision left
		ResOut[3] = collision center
		ResOut[3] = collision right
*/

// Checklist
// [x] Set voltage-Freq 
// [x] Flash
// [x] open filesystem on flash
// [x] RAM
// [x] UART conf
// [x] Camera
// [x] open cluster
// [x] CNN task
// [x] allocate CNN output tensor
// [x] Network setup
// [x] while 1
void body()
{
    pi_fs_file_t *file;
    struct pi_device fs;
    struct pi_device flash;
    struct pi_hyperflash_conf flash_conf;
    struct pi_readfs_conf conf0;

	// Voltage and Frequency settings
	uint32_t voltage =1200;
	PMU_set_voltage(voltage, 0);
	pi_time_wait_us(10000);
	pi_freq_set(PI_FREQ_DOMAIN_FC, FREQ_FC*1000*1000);
	pi_time_wait_us(10000);
	pi_freq_set(PI_FREQ_DOMAIN_CL, FREQ_CL*1000*1000);
	pi_time_wait_us(10000);
	printf("Set VDD voltage as %.2f, FC Frequency as %d MHz, CL Frequency = %d MHz\n", 
		(float)voltage/1000, FREQ_FC, FREQ_CL);

	// Initialize the Flash
    pi_hyperflash_conf_init(&flash_conf);
    pi_open_from_conf(&flash, &flash_conf);
    if (pi_flash_open(&flash))
    {
        printf("Error flash open ! \n");
        pmsis_exit(-3);
    }

    // Open filesystem on flash
    pi_readfs_conf_init(&conf0);
    conf0.fs.flash = &flash;
    pi_open_from_conf(&fs, &conf0);
    if (pi_fs_mount(&fs))
    {
        printf("Error FS mounting ! \n");
        pmsis_exit(-2);
    }

	// Initialize the ram 
  	struct pi_hyperram_conf hyper_conf;
  	pi_hyperram_conf_init(&hyper_conf);
  	pi_open_from_conf(&HyperRam, &hyper_conf);
	if (pi_ram_open(&HyperRam))
	{
		printf("Error ram open !\n");
		pmsis_exit(-3);
	}

	// UART Configuration
	struct pi_device uart;
    struct pi_uart_conf conf_uart;
    pi_uart_conf_init(&conf_uart);
    conf_uart.enable_tx = 1;
    conf_uart.enable_rx = 0;
    conf_uart.baudrate_bps = 115200;
    pi_open_from_conf(&uart, &conf_uart);
    if (pi_uart_open(&uart))
    {
        printf("Uart open failed !\n");
        pmsis_exit(-1);
    }

    //configure LED
    pi_gpio_pin_configure(&gpio_device, 2, PI_GPIO_OUTPUT);
    LED_ON;

    // Open the Himax camera
    open_camera();

	////////////////////////////////////////////////////////////////////////////////


	// Network Constructor
	char* input_image_buffer = network_setup();
	printf("Network has been set up\n");
    // printf("---------------------L2_input addr %ld \n", input_image_buffer);

	// Allocate the output tensor
	ResOut = (int32_t *) pi_l2_malloc(CNN_OUTPUTS*sizeof(int32_t));
	if (ResOut==0) {
		printf("Failed to allocate Memory for Result (%ld bytes)\n", CNN_OUTPUTS*sizeof(int32_t));
		return 1;
	}

	// CNN task setup
	struct pi_cluster_task cluster_task = {0};
	cluster_task.entry = (void *) pulp_parallel; // function call in network.c
	cluster_task.stack_size = 4096;
	cluster_task.slave_stack_size = 3072;
	cluster_task.arg = NULL;

	// Open the cluster
	struct pi_device cluster_dev = {0};
	struct pi_cluster_conf conf;	
	pi_cluster_conf_init(&conf);
	conf.id=0;
	pi_open_from_conf(&cluster_dev, &conf);
	if (pi_cluster_open(&cluster_dev))
		return -1;

	printf("Network Running...\n");


    #ifdef JPEG_STREAMER
        if (open_wifi(&wifi))
        {
          printf("Failed to open wifi\n");
          return -1;
        }
        printf("Opened WIFI\n");



        streamer = open_streamer("camera");
        if (streamer == NULL)
          return -1;

        pi_buffer_init(&buffer, PI_BUFFER_TYPE_L2, input_image_buffer);
        pi_buffer_set_format(&buffer, STREAM_WIDTH, STREAM_HEIGHT, 1, PI_BUFFER_FORMAT_GRAY);
        printf("Opened streamer\n");

    #endif


	// // DEBUG camera
	// uint32_t idx = 0;
	// char ImageName[50];
	// while(1){ // DEBUG CAMERA ACQUISITIONS
	// 	// Start camera acquisition
	// 	sprintf(ImageName, "../../../output_%d.ppm", idx);
	// 	pi_camera_control(&camera, PI_CAMERA_CMD_START, 0);
	// 	pi_camera_capture(&camera, input_image_buffer, BUFF_SIZE);
	// 	pi_camera_control(&camera, PI_CAMERA_CMD_STOP, 0);
	// 	image_crop(input_image_buffer, input_image_buffer);
	// 	WriteImageToFile(ImageName, INPUT_WIDTH, INPUT_HEIGHT,sizeof(uint8_t), input_image_buffer, GRAY_SCALE_IO);
	// 	idx++;
	// }

#ifdef PERF
	float perf_cyc;
	float perf_s;	
	pi_perf_conf(1<<PI_PERF_CYCLES); 
#endif    

    printf("While loop...\n");
	while(1){
		start_perf_counter();

        LED_OFF;
		// Start camera acquisition
		pi_camera_control(&camera, PI_CAMERA_CMD_START, 0);
		pi_camera_capture(&camera, input_image_buffer, BUFF_SIZE);

        #ifdef JPEG_STREAMER
            frame_streamer_send(streamer, &buffer);
        #endif

		// Crop the image
		#ifndef IMAV
			image_crop(input_image_buffer, input_image_buffer);
		#endif
		pi_camera_control(&camera, PI_CAMERA_CMD_STOP, 0);

        LED_ON;
  		// Run CNN inference
		pi_cluster_send_task_to_cl(&cluster_dev, &cluster_task);
      	// printf("main.c: Steering Angle: %d, Collision: %d \n",  ResOut[0], ResOut[1]);

		// prepare data for UART send
		for(int i=0; i<CNN_OUTPUTS; i++){
			data_to_send[i] = ResOut[i];
		}

		/* UART synchronous send */
	    // pi_uart_write(&uart, (char *) data_to_send, CNN_OUTPUTS*4);

		/* UART asynchronous send */
		pi_task_t wait_task2 = {0};
	    pi_task_block(&wait_task2);
	    pi_uart_write_async(&uart, (char *) data_to_send, CNN_OUTPUTS*4, &wait_task2);
		// pi_task_wait_on(&wait_task2);

		end_perf_counter(true);
	}

	// close the cluster
	pi_cluster_close(&cluster_dev);
	pmsis_exit(0);
	return 0;
}





int main(void)
{
    printf("\n\n\t *** DroNet on GAP ***\n");
    return pmsis_kickoff((void *) body);
}



// // on fabric controller
// int main () {
//   	network_setup();
//   	network_run_FabricController();
// }














// void run_L3_weights()
// {

// 	PMU_set_voltage(1000, 0);
// 	pi_time_wait_us(10000);
// 	pi_freq_set(PI_FREQ_DOMAIN_FC, FREQ_FC*1000*1000);
// 	pi_time_wait_us(10000);
// 	pi_freq_set(PI_FREQ_DOMAIN_CL, FREQ_CL*1000*1000);
// 	pi_time_wait_us(10000);


// 	unsigned int L3_weights_size = network_setup();
// 	int arg[1];
// 	arg[0] = (unsigned int) L3_weights_size;

// 	struct pi_device cluster_dev = {0};
// 	struct pi_cluster_conf conf;
// 	struct pi_cluster_task cluster_task = {0};
// 	// task parameters allocation
// 	pi_cluster_task(&cluster_task, pulp_parallel, arg);
// 	cluster_task.stack_size = 4096;
// 	cluster_task.slave_stack_size = 3072;
// 	// First open the cluster
// 	pi_cluster_conf_init(&conf);
// 	conf.id=0;
// 	pi_open_from_conf(&cluster_dev, &conf);
// 	if (pi_cluster_open(&cluster_dev))
// 		return -1;
// 	// Then offload an entry point, this will get executed on the cluster controller
// 	pi_cluster_send_task_to_cl(&cluster_dev, &cluster_task);
// 	// closing of the cluster
// 	pi_cluster_close(&cluster_dev);
// 	pmsis_exit(0);
// 	return 0;
// }
