#include "bnn.h"
#include "layer.h"
#include "dimension_def.h"

using namespace std;

static  int8 msb_fmap_tile_buffer_0[BATCH_SIZE][CHANNEL_IN_T][WIDTH][WIDTH];	// activation/error on-chip
static  int8 msb_fmap_tile_buffer_1[BATCH_SIZE][CHANNEL_IN_T][WIDTH][WIDTH];	// activation/error on-chip
static  int8 lsb_fmap_tile_buffer[BATCH_SIZE][CHANNEL_IN_T][WIDTH][WIDTH];		// shortcut activation/error on-chip

static	int8 conv_3x3_weight_tile_buffer[CHANNEL_OUT_T][CHANNEL_IN_T][3][3];
static	int8 conv_1x1_weight_tile_buffer[CHANNEL_OUT_T][CHANNEL_IN_T];

static	int8 grad_buf_t0[CHANNEL_OUT_T][CHANNEL_IN_T][3][3];					// weight_3x3 gradient on-chip
static	int8 grad_buf_t1[CHANNEL_OUT_T][CHANNEL_IN_T];							// weight_1x1 gradient on-chip

static  int8 pool_out_buf[BATCH_SIZE][512];										// AvgPool buffer
static  int8 linear_weight_tile_buffer[10][512];
static  int8 linear_out_buf[BATCH_SIZE][10];									// FC buffer

static	int8 gamma[CHANNEL_OUT_T];
static	int8 beta[CHANNEL_OUT_T];
static	int8 grad_gamma[CHANNEL_OUT_T];
static	int8 grad_beta[CHANNEL_OUT_T];

//--------------------
//  Top Function 
//--------------------
void FracNet_T(
	int8 image[BATCH_SIZE][3][32][32],
	int8 output[BATCH_SIZE][10],

	int8 conv_3x3_weight_all[NUM_3x3_WT][CHANNEL_OUT_T][CHANNEL_IN_T][3][3],
	int8 conv_1x1_weight_all[NUM_1x1_WT][CHANNEL_OUT_T][CHANNEL_IN_T],
	int8 linear_weight[10][512],

	int8 out_buf_t0[NUM_ACT][BATCH_SIZE][CHANNEL_OUT_T][WIDTH][WIDTH],		// BN output activation
    int8 out_buf_t1[NUM_ACT][BATCH_SIZE][CHANNEL_OUT_T][WIDTH][WIDTH],		// Conv output activation

    int1 relu_mask[NUM_ACT][BATCH_SIZE][CHANNEL_OUT_T][WIDTH][WIDTH]		// relu mask for backprop
)
{
#pragma HLS INTERFACE m_axi depth=12288 port=image offset=slave bundle=IMG	// 4*3*32*32 = 12288
#pragma HLS INTERFACE m_axi depth=40 port=output offset=slave bundle=RESULT	// 4*10

#pragma HLS INTERFACE m_axi depth=14008320 port=conv_3x3_weight_all offset=slave bundle=conv_3x3_weight_all	// 300*64*64*3*3 = 11059200
#pragma HLS INTERFACE m_axi depth=176128 port=conv_1x1_weight_all offset=slave bundle=conv_1x1_weight_all	// 43*64*64 = 176128
#pragma HLS INTERFACE m_axi depth=5120 port=linear_weight offset=slave bundle=linear_weight					// 8*10*64 = 5120

#pragma HLS INTERFACE m_axi depth=17284608 port=out_buf_t0 offset=slave bundle=out_buf_t0					// 62*4*64*33*33 = 17284608
#pragma HLS INTERFACE m_axi depth=17284608 port=out_buf_t1 offset=slave bundle=out_buf_t1
/* #pragma HLS INTERFACE m_axi depth=46835712 port=relu_mask offset=slave bundle=relu_mask */

#pragma HLS INTERFACE s_axilite port=return bundle=CTRL

// instance allocation
#pragma HLS ALLOCATION function instances=bn_relu limit=1
#pragma HLS ALLOCATION function instances=bn_relu_bp limit=1
#pragma HLS ALLOCATION function instances=shortcut limit=1

#pragma HLS ALLOCATION function instances=avgpool limit=1
#pragma HLS ALLOCATION function instances=FC limit=1

#pragma HLS ALLOCATION function instances=conv_3x3 limit=1
#pragma HLS ALLOCATION function instances=conv_1x1 limit=1

#pragma HLS ALLOCATION function instances=conv_3x3_rot_bp limit=1
#pragma HLS ALLOCATION function instances=conv_1x1_rot_bp limit=1
#pragma HLS ALLOCATION function instances=conv_3x3_grad limit=1
#pragma HLS ALLOCATION function instances=conv_1x1_grad limit=1

#pragma HLS ALLOCATION function instances=SGD_WU_3x3 limit=1
#pragma HLS ALLOCATION function instances=SGD_WU_1x1 limit=1

/*
// array partition
#pragma HLS ARRAY_PARTITION variable=msb_fmap_tile_buffer_0 complete dim=2
#pragma HLS ARRAY_PARTITION variable=msb_fmap_tile_buffer_1 complete dim=2
#pragma HLS ARRAY_PARTITION variable=lsb_fmap_tile_buffer complete dim=2

#pragma HLS ARRAY_PARTITION variable=conv_3x3_weight_tile_buffer complete dim=1
#pragma HLS ARRAY_PARTITION variable=conv_3x3_weight_tile_buffer complete dim=2
#pragma HLS ARRAY_PARTITION variable=conv_1x1_weight_tile_buffer complete dim=1
#pragma HLS ARRAY_PARTITION variable=conv_1x1_weight_tile_buffer complete dim=2

// #pragma HLS ARRAY_PARTITION variable=pool_out_buf complete dim=2
#pragma HLS ARRAY_PARTITION variable=linear_weight_tile_buffer complete dim=2

#pragma HLS ARRAY_PARTITION variable=grad_buf_t0 complete dim=1
#pragma HLS ARRAY_PARTITION variable=grad_buf_t0 complete dim=2
#pragma HLS ARRAY_PARTITION variable=grad_buf_t1 complete dim=1
#pragma HLS ARRAY_PARTITION variable=grad_buf_t1 complete dim=2

#pragma HLS ARRAY_PARTITION variable=gamma complete dim=1
#pragma HLS ARRAY_PARTITION variable=beta complete dim=1
#pragma HLS ARRAY_PARTITION variable=grad_gamma complete dim=1
#pragma HLS ARRAY_PARTITION variable=grad_beta complete dim=1
*/

	int H_fmap_in, H_fmap_out, in_channels, in_channels_after_pack, out_channels_after_pack;
    int out_channels, out_channel_start, stride, conv_3x3_weight_ptr, conv_1x1_weight_ptr, fc_weight_ptr, ini;
	int1 ctrl_sc, ctrl_fc, ctrl_avgpool;


//////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////		Forward path		//////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////


	////////////////////////////////////////////////
	//////////// GET IMAGE /////////////////////////
	////////////////////////////////////////////////

	ini = 0;
	ctrl_sc = 1; // if ctrl_sc=1, generate and send out_copy into DDR

	LOOP_GetImg:
	for (int row = 0; row < 32; row ++) {
		for (int col = 0; col < 32; col ++) {
#pragma HLS pipeline
			for (int b = 0; b < BATCH_SIZE; b ++) {
				for (int c = 0; c < 3; c ++) {
					msb_fmap_tile_buffer_1[b][c][row][col] = image[b][c][row][col];
					out_buf_t1[ini][b][c][row][col] = msb_fmap_tile_buffer_1[b][c][row][col];	// store image as the first input activation
				}
			}
		}
	}

	////////////////////////////////////////////////
	/////////// Conv 1 + bn 1 + relu 1 /////////////
	////////////////////////////////////////////////

	in_channels = 3;
	in_channels_after_pack = in_channels/CHANNEL_IN_T;
	out_channels = 64;
	H_fmap_in =32;
	H_fmap_out = 32;
	stride = 1;
	conv_3x3_weight_ptr = 0;

    LOOP_Conv1:
	ini += 1;	// ini = 1
	for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
		conv_3x3_weight_ptr += 1;

		load_conv_3x3_weights(
			conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
		);
		conv_3x3(
			msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t0[ini],
			stride, H_fmap_out, 0 // c_in
		);
		bn_relu(
			msb_fmap_tile_buffer_0, msb_fmap_tile_buffer_1, out_buf_t1[ini], relu_mask[ini],
			gamma, beta,
			H_fmap_out
		);
	}

	////////////////////////////////////////////////
	//////////// LAYER 1 ///////////////////////////
	////////////////////////////////////////////////

	H_fmap_in = 32;
	H_fmap_out = 32;
	in_channels = 64;
	in_channels_after_pack = in_channels/CHANNEL_IN_T;
	out_channels = 64;
	stride = 1;

	////////////////////////////////////////////////
	//////////// layer1_0 PG1 //////////////////////
	LOOP_layer1_0_Conv1:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 2
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_3x3_weight_ptr += 1;

			// lsb_fmap[ini] = msb_fmap[ini]; 	// identity branch
			identity_shortcut(
				msb_fmap_tile_buffer_1, lsb_fmap_tile_buffer,
				H_fmap_in
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3(
				msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn_relu(
				msb_fmap_tile_buffer_0, msb_fmap_tile_buffer_1, out_buf_t1[ini], relu_mask[ini],
				gamma, beta,
				H_fmap_out
			);
		}
    }

	////////////////////////////////////////////////
	//////////// layer1_0 PG2 //////////////////////
	LOOP_layer1_0_Conv2:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 3
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_3x3_weight_ptr += 1;

			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3(
				msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn_relu(
				msb_fmap_tile_buffer_0, msb_fmap_tile_buffer_1, out_buf_t1[ini], relu_mask[ini],
				gamma, beta,
				H_fmap_out
			);
			shortcut(
				msb_fmap_tile_buffer_1, lsb_fmap_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t1[ini],
				H_fmap_out, ctrl_sc	// generate and send out_copy into DDR
			);
		}
    }

	////////////////////////////////////////////////
	//////////// layer1_1 PG1 //////////////////////
	LOOP_layer1_1_Conv1:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 4
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_3x3_weight_ptr += 1;

			// lsb_fmap[ini] = msb_fmap[ini]; 	// identity branch
			identity_shortcut(
				msb_fmap_tile_buffer_0, lsb_fmap_tile_buffer,
				H_fmap_in
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3(
				msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn_relu(
				msb_fmap_tile_buffer_1, msb_fmap_tile_buffer_0, out_buf_t1[ini], relu_mask[ini],
				gamma, beta,
				H_fmap_out
			);
		}
    }

	////////////////////////////////////////////////
	//////////// layer1_1 PG2 //////////////////////
	LOOP_layer1_1_Conv2:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 5
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_3x3_weight_ptr += 1;

			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3(
				msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn_relu(
				msb_fmap_tile_buffer_1, msb_fmap_tile_buffer_0, out_buf_t1[ini], relu_mask[ini],
				gamma, beta,
				H_fmap_out
			);
			shortcut(
				msb_fmap_tile_buffer_0, lsb_fmap_tile_buffer, msb_fmap_tile_buffer_1, out_buf_t1[ini],
				H_fmap_out, ctrl_sc
			);
		}
    }

	////////////////////////////////////////////////
	//////////// LAYER 2 Downsample ////////////////
	////////////////////////////////////////////////

	H_fmap_in = 32;
	H_fmap_out = 16;
	in_channels = 64;
	in_channels_after_pack = in_channels/CHANNEL_IN_T;
	out_channels = 128;
	stride = 2;
	conv_1x1_weight_ptr = 0;

	////////////////////////////////////////////////
	//////////// layer2_0 shortcut (conv+bn) ///////
	LOOP_layer2_0_ConvSC:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 6
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_1x1_weight_ptr += 1;

			load_conv_1x1_weights(
				conv_1x1_weight_tile_buffer, conv_1x1_weight_all[conv_1x1_weight_ptr]
			);
			conv_1x1(
				msb_fmap_tile_buffer_1, conv_1x1_weight_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn(
				msb_fmap_tile_buffer_0, lsb_fmap_tile_buffer,	// conv+bn shortcut
				gamma, beta,
				H_fmap_out
			);
		}
    }

	////////////////////////////////////////////////
	//////////// layer2_0 PG1 //////////////////////
	LOOP_layer2_0_Conv1:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 7
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_3x3_weight_ptr += 1;

			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3(
				msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn_relu(
				msb_fmap_tile_buffer_0, msb_fmap_tile_buffer_1, out_buf_t1[ini], relu_mask[ini],
				gamma, beta,
				H_fmap_out
			);
		}
    }

	////////////////////////////////////////////////
	//////////// LAYER 2 ///////////////////////////
	////////////////////////////////////////////////

	H_fmap_in = 16;
	H_fmap_out = 16;
	in_channels = 128;
	in_channels_after_pack = in_channels/CHANNEL_IN_T;
	out_channels = 128;
	stride = 1;

	////////////////////////////////////////////////
	//////////// layer2_0 PG2 //////////////////////
	LOOP_layer2_0_Conv2:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 8
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_3x3_weight_ptr += 1;

			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3(
				msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn_relu(
				msb_fmap_tile_buffer_0, msb_fmap_tile_buffer_1, out_buf_t1[ini], relu_mask[ini],
				gamma, beta,
				H_fmap_out
			);
			shortcut(
				msb_fmap_tile_buffer_1, lsb_fmap_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t1[ini],
				H_fmap_out, ctrl_sc
			);
		}
	}

	////////////////////////////////////////////////
	//////////// layer2_1 PG1 //////////////////////
	LOOP_layer2_1_Conv1:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 9
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_3x3_weight_ptr += 1;

			// lsb_fmap[ini] = msb_fmap[ini]; 	// identity branch
			identity_shortcut(
				msb_fmap_tile_buffer_0, lsb_fmap_tile_buffer,
				H_fmap_in
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3(
				msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn_relu(
				msb_fmap_tile_buffer_1, msb_fmap_tile_buffer_0, out_buf_t1[ini], relu_mask[ini],
				gamma, beta,
				H_fmap_out
			);
		}
	}

	////////////////////////////////////////////////
	//////////// layer2_1 PG2 //////////////////////
	LOOP_layer2_1_Conv2:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 10
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_3x3_weight_ptr += 1;

			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3(
				msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn_relu(
				msb_fmap_tile_buffer_1, msb_fmap_tile_buffer_0, out_buf_t1[ini], relu_mask[ini],
				gamma, beta,
				H_fmap_out
			);
			shortcut(
				msb_fmap_tile_buffer_0, lsb_fmap_tile_buffer, msb_fmap_tile_buffer_1, out_buf_t1[ini],
				H_fmap_out, ctrl_sc
			);
		}
	}


	////////////////////////////////////////////////
	//////////// LAYER 3 Downsample ////////////////
	////////////////////////////////////////////////

	H_fmap_in = 16;
	H_fmap_out = 8;
	in_channels = 128;
	in_channels_after_pack = in_channels/CHANNEL_IN_T;
	out_channels = 256;
	stride = 2;

	////////////////////////////////////////////////
	//////////// layer3_0 shortcut (conv+bn) ///////
	LOOP_layer3_0_ConvSC:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 11
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_1x1_weight_ptr += 1;

			load_conv_1x1_weights(
				conv_1x1_weight_tile_buffer, conv_1x1_weight_all[conv_1x1_weight_ptr]
			);
			conv_1x1(
				msb_fmap_tile_buffer_1, conv_1x1_weight_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn(
				msb_fmap_tile_buffer_0, lsb_fmap_tile_buffer,	// conv+bn shortcut
				gamma, beta,
				H_fmap_out
			);
		}
	}

	////////////////////////////////////////////////
	//////////// layer3_0 PG1 //////////////////////
	LOOP_layer3_0_Conv1:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 12
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_3x3_weight_ptr += 1;

			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3(
				msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn_relu(
				msb_fmap_tile_buffer_0, msb_fmap_tile_buffer_1, out_buf_t1[ini], relu_mask[ini],
				gamma, beta,
				H_fmap_out
			);
		}
	}

	////////////////////////////////////////////////
	//////////// LAYER 3 ///////////////////////////
	////////////////////////////////////////////////

	H_fmap_in = 8;
	H_fmap_out = 8;
	in_channels = 256;
	in_channels_after_pack = in_channels/CHANNEL_IN_T;
	out_channels = 256;
	stride = 1;

	////////////////////////////////////////////////
	//////////// layer3_0 PG2 //////////////////////
	LOOP_layer3_0_Conv2:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 13
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_3x3_weight_ptr += 1;

			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3(
				msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn_relu(
				msb_fmap_tile_buffer_0, msb_fmap_tile_buffer_1, out_buf_t1[ini], relu_mask[ini],
				gamma, beta,
				H_fmap_out
			);
			shortcut(
				msb_fmap_tile_buffer_1, lsb_fmap_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t1[ini],
				H_fmap_out, ctrl_sc
			);
		}
	}

	////////////////////////////////////////////////
	//////////// layer3_1 PG1 //////////////////////
	LOOP_layer3_1_Conv1:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 14
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_3x3_weight_ptr += 1;

			// lsb_fmap[ini] = msb_fmap[ini];	// identity branch
			identity_shortcut(
				msb_fmap_tile_buffer_0, lsb_fmap_tile_buffer,
				H_fmap_in
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3(
				msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn_relu(
				msb_fmap_tile_buffer_1, msb_fmap_tile_buffer_0, out_buf_t1[ini], relu_mask[ini],
				gamma, beta,
				H_fmap_out
			);
		}
	}

	////////////////////////////////////////////////
	//////////// layer3_1 PG2 //////////////////////
	LOOP_layer3_1_Conv2:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 15
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_3x3_weight_ptr += 1;

			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3(
				msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn_relu(
				msb_fmap_tile_buffer_1, msb_fmap_tile_buffer_0, out_buf_t1[ini], relu_mask[ini],
				gamma, beta,
				H_fmap_out
			);
			shortcut(
				msb_fmap_tile_buffer_0, lsb_fmap_tile_buffer, msb_fmap_tile_buffer_1, out_buf_t1[ini],
				H_fmap_out, ctrl_sc
			);	// CHANNEL_OUT_T = CHANNEL_IN_T
		}
	}

	////////////////////////////////////////////////
	//////////// LAYER 4 Downsample ////////////////
	////////////////////////////////////////////////

	H_fmap_in = 8;
	H_fmap_out = 4;
	in_channels = 256;
	in_channels_after_pack = in_channels/CHANNEL_IN_T;
	out_channels = 512;
	stride = 2;

	////////////////////////////////////////////////
	//////////// layer4_0 shortcut (conv+bn) ///////
	LOOP_layer4_0_ConvSC:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 16
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_1x1_weight_ptr += 1;

			load_conv_1x1_weights(
				conv_1x1_weight_tile_buffer, conv_1x1_weight_all[conv_1x1_weight_ptr]
			);
			conv_1x1(
				msb_fmap_tile_buffer_1, conv_1x1_weight_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn(
				msb_fmap_tile_buffer_0, lsb_fmap_tile_buffer,	// conv+bn shortcut
				gamma, beta,
				H_fmap_out
			);
		}
	}

	////////////////////////////////////////////////
	//////////// layer4_0 PG1 //////////////////////
	LOOP_layer4_0_Conv1:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 17
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_3x3_weight_ptr += 1;

			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3(
				msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn_relu(
				msb_fmap_tile_buffer_0, msb_fmap_tile_buffer_1, out_buf_t1[ini], relu_mask[ini],
				gamma, beta,
				H_fmap_out
			);
		}
	}

	////////////////////////////////////////////////
	//////////// LAYER 4 ///////////////////////////
	////////////////////////////////////////////////

	H_fmap_in = 4;
	H_fmap_out = 4;
	in_channels = 512;
	in_channels_after_pack = in_channels/CHANNEL_IN_T;
	out_channels = 512;
	stride = 1;

	////////////////////////////////////////////////
	//////////// layer4_0 PG2 //////////////////////
	LOOP_layer4_0_Conv2:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 18
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_3x3_weight_ptr += 1;

			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3(
				msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn_relu(
				msb_fmap_tile_buffer_0,  msb_fmap_tile_buffer_1, out_buf_t1[ini], relu_mask[ini],
				gamma, beta,
				H_fmap_out
			);
			shortcut(
				msb_fmap_tile_buffer_1, lsb_fmap_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t1[ini],
				H_fmap_out, ctrl_sc
			);	// CHANNEL_OUT_T = CHANNEL_IN_T
		}
	}

	////////////////////////////////////////////////
	//////////// layer4_1 PG1 //////////////////////
	LOOP_layer4_1_Conv1:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 19
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_3x3_weight_ptr += 1;

			// lsb_fmap[ini] = msb_fmap[ini];	// identity branch
			identity_shortcut(
				msb_fmap_tile_buffer_0, lsb_fmap_tile_buffer,
				H_fmap_in
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3(
				msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn_relu(
				msb_fmap_tile_buffer_1, msb_fmap_tile_buffer_0, out_buf_t1[ini], relu_mask[ini],
				gamma, beta,
				H_fmap_out
			);
		}
	}

	////////////////////////////////////////////////
	//////////// layer4_1 PG2 //////////////////////
	LOOP_layer4_1_Conv2:
	for (int c_in = 0; c_in < in_channels/CHANNEL_IN_T; c_in ++) {
		ini += 1;	// ini = 20
		for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
			conv_3x3_weight_ptr += 1;

			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3(
				msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1, out_buf_t0[ini],
				stride, H_fmap_out, c_in
			);
			bn_relu(
				msb_fmap_tile_buffer_1, msb_fmap_tile_buffer_0, out_buf_t1[ini], relu_mask[ini],
				gamma, beta,
				H_fmap_out
			);
			shortcut(
				msb_fmap_tile_buffer_0, lsb_fmap_tile_buffer, msb_fmap_tile_buffer_1, out_buf_t1[ini],
				H_fmap_out, ctrl_sc
			);
		}
	}

	////////////////////////////////////////////////
	//////////// AvgPool and FC // /////////////////

	// initialize buffers for pooling and FC
	AvgPool_FC_buf_init:
	for (int b = 0; b < BATCH_SIZE; b ++) {
		for (int i = 0; i < 512; i ++) {
			pool_out_buf[b][i] = 0;
		}
#pragma HLS pipeline
		for (int j = 0; j < 10; j ++) {
			linear_out_buf[b][j] = 0;
		}
	}

	// avgpool
	LOOP_AvgPool:
	ctrl_avgpool = 0;
	for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
		avgpool(
			msb_fmap_tile_buffer_1, pool_out_buf, ctrl_avgpool, c_out
		);
	}

	// FC
	LOOP_FC:
	fc_weight_ptr = 0;
	ctrl_fc = 0;
	load_fc_weights(
		linear_weight_tile_buffer, linear_weight
	);
	FC(
		pool_out_buf, linear_weight_tile_buffer, linear_out_buf, ctrl_fc
	);

	write_output:
	for (int j = 0; j < 10; j ++) {
#pragma HLS pipeline
		for (int b = 0; b < BATCH_SIZE; b ++) {
			output[b][j] = linear_out_buf[b][j];
		}
	}

	////////////////////////////////////////////////
	//////////// CrossEntropy loss /////////////////
	int8 error[BATCH_SIZE][10];
	int8 labels[10];

	error_calc:
	// MSE for simplicity

		for (int j = 0; j < 10; j ++) {
			for (int b = 0; b < BATCH_SIZE; b ++) {
			error[b][j] = 2 * (linear_out_buf[b][j] - labels[j]);
		}
	}


//////////////////////////////////////////////////////////////////////////////////////
//////////////		Backward path and Gradient Calc & Weight update		//////////////
//////////////////////////////////////////////////////////////////////////////////////


	////////////////////////////////////////////////
	//////////// AvgPool and FC // /////////////////

	// initialize buffers for pooling and FC
	AvgPool_buf_init_bp:
	for (int i = 0; i < 512; i ++) {
#pragma HLS pipeline
		for (int b = 0; b < BATCH_SIZE; b ++) {
			pool_out_buf[b][i] = 0;
		}
	}

	// FC_bp
	LOOP_FC_bp:
	ctrl_fc = 1;
	FC(
		pool_out_buf, linear_weight_tile_buffer, error, ctrl_fc
	);

	// avgpool_bp
	LOOP_AvgPool_bp:
	ctrl_avgpool = 1;
	for (int c_out = 0; c_out < out_channels/CHANNEL_OUT_T; c_out ++) {
		avgpool(
			msb_fmap_tile_buffer_1, pool_out_buf, ctrl_avgpool, c_out
		);
	}


	////////////////////////////////////////////////
	//////////// LAYER 4 ///////////////////////////
	////////////////////////////////////////////////

	H_fmap_in = 4;
	H_fmap_out = 4;
	in_channels = 512;
	out_channels_after_pack = out_channels/CHANNEL_OUT_T;
	out_channels = 512;
	stride = 1;

	ctrl_sc = 0; // do not generate output_copy of shortcut

	////////////////////////////////////////////////
	//////////// layer4_1 PG2 //////////////////////
	LOOP_layer4_1_Conv2_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 20
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_3x3_weight_ptr -= 1;

			// lsb_fmap[ini] = msb_fmap[ini];	// identity branch
			identity_shortcut(
				msb_fmap_tile_buffer_1, lsb_fmap_tile_buffer,
				H_fmap_in
			);
			bn_relu_bp(
				msb_fmap_tile_buffer_1, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_0,
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3_rot_bp(
				msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_3x3_weight_grad_cal
			conv_3x3_grad(
				out_buf_t1[ini - out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_0, grad_buf_t0,
				stride, H_fmap_in
			);
			SGD_WU_3x3(
				grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			// end gradient calculation
			//////////////////////////
		}
    }

	////////////////////////////////////////////////
	//////////// layer4_1 PG1 //////////////////////
	LOOP_layer4_1_Conv1_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 19
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_3x3_weight_ptr -= 1;

			bn_relu_bp(
				msb_fmap_tile_buffer_1, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_0,
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3_rot_bp(
				msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_3x3_weight_grad_cal
			///////////////////////////
			conv_3x3_grad(
				out_buf_t1[ini - out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_0, grad_buf_t0,
				stride, H_fmap_in
			);
			SGD_WU_3x3(
				grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
			shortcut(
				msb_fmap_tile_buffer_1, lsb_fmap_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t0[ini],	// DDR not used
				H_fmap_out, ctrl_sc
			);
		}
    }

	////////////////////////////////////////////////
	//////////// layer4_0 PG2 //////////////////////
	LOOP_layer4_0_Conv2_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 18
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_3x3_weight_ptr -= 1;

			bn_relu_bp(
				msb_fmap_tile_buffer_0, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_1,
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3_rot_bp(
				msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_0,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_3x3_weight_grad_cal
			conv_3x3_grad(
				out_buf_t1[ini - out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_1, grad_buf_t0,
				stride, H_fmap_in
			);
			SGD_WU_3x3(
				grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
		}
    }

	////////////////////////////////////////////////
	//////////// LAYER 4 Upsample //////////////////
	////////////////////////////////////////////////

	H_fmap_in = 4;
	H_fmap_out = 8;
	in_channels = 512;
	out_channels_after_pack = out_channels/CHANNEL_OUT_T;
	out_channels = 256;
	stride = 2;

	////////////////////////////////////////////////
	//////////// layer4_0 PG1 //////////////////////
	LOOP_layer4_0_Conv1_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 17
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_3x3_weight_ptr -= 1;

			bn_relu_bp(
				msb_fmap_tile_buffer_0, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_1,
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3_rot_bp(
				msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, lsb_fmap_tile_buffer,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_3x3_weight_grad_cal
			conv_3x3_grad(
				out_buf_t1[ini - 2*out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_1, grad_buf_t0,
				stride, H_fmap_in
			);
			SGD_WU_3x3(
				grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
		}
    }

	////////////////////////////////////////////////
	//////////// layer4_0 shortcut (conv+bn) ///////
	LOOP_layer4_0_ConvSC_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 16
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_1x1_weight_ptr -= 1;

			load_conv_1x1_weights(
				conv_1x1_weight_tile_buffer, conv_1x1_weight_all[conv_1x1_weight_ptr]
			);
			bn_bp(
				msb_fmap_tile_buffer_0, out_buf_t0[ini], msb_fmap_tile_buffer_1,	// conv+bn shortcut
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			conv_1x1_rot_bp(	// note the index of shortcut input
				msb_fmap_tile_buffer_1, conv_1x1_weight_tile_buffer, msb_fmap_tile_buffer_0,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_1x1_weight_grad_cal
			conv_1x1_grad(
				out_buf_t1[ini - out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_1, grad_buf_t1,
				stride, H_fmap_in
			);
			SGD_WU_1x1(
				grad_buf_t1, conv_1x1_weight_tile_buffer, conv_1x1_weight_all[conv_1x1_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
			shortcut(
				msb_fmap_tile_buffer_0, lsb_fmap_tile_buffer, msb_fmap_tile_buffer_1, out_buf_t0[ini],	// DDR not used
				H_fmap_out, ctrl_sc
			);
		}
    }


	////////////////////////////////////////////////
	//////////// LAYER 3 ///////////////////////////
	////////////////////////////////////////////////

	H_fmap_in = 8;
	H_fmap_out = 8;
	in_channels = 256;
	out_channels_after_pack = out_channels/CHANNEL_OUT_T;
	out_channels = 256;
	stride = 1;

	////////////////////////////////////////////////
	//////////// layer3_1 PG2 //////////////////////
	LOOP_layer3_1_Conv2_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 15
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_3x3_weight_ptr -= 1;

			// lsb_fmap[ini] = msb_fmap[ini];	// identity branch
			identity_shortcut(
				msb_fmap_tile_buffer_1, lsb_fmap_tile_buffer,
				H_fmap_in
			);
			bn_relu_bp(
				msb_fmap_tile_buffer_1, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_0,
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3_rot_bp(
				msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_3x3_weight_grad_cal
			conv_3x3_grad(
				out_buf_t1[ini - out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_0, grad_buf_t0,
				stride, H_fmap_in
			);
			SGD_WU_3x3(
				grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
		}
	}

	////////////////////////////////////////////////
	//////////// layer3_1 PG1 //////////////////////
	LOOP_layer3_1_Conv1_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 14
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_3x3_weight_ptr -= 1;

			bn_relu_bp(
				msb_fmap_tile_buffer_1, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_0,
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3_rot_bp(
				msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_3x3_weight_grad_cal
			conv_3x3_grad(
				out_buf_t1[ini - out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_0, grad_buf_t0,
				stride, H_fmap_in
			);
			SGD_WU_3x3(
				grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
			shortcut(
				msb_fmap_tile_buffer_1, lsb_fmap_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t0[ini],	// DDR not used
				H_fmap_out, ctrl_sc
			);
		}
	}

	////////////////////////////////////////////////
	//////////// layer3_0 PG2 //////////////////////
	LOOP_layer3_0_Conv2_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 13
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_3x3_weight_ptr -= 1;

			bn_relu_bp(
				msb_fmap_tile_buffer_0, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_1,
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3_rot_bp(
				msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_0,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_3x3_weight_grad_cal
			conv_3x3_grad(
				out_buf_t1[ini - out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_1, grad_buf_t0,
				stride, H_fmap_in
			);
			SGD_WU_3x3(
				grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
		}
	}

	////////////////////////////////////////////////
	//////////// LAYER 3 Upsample //////////////////
	////////////////////////////////////////////////

	H_fmap_in = 8;
	H_fmap_out = 16;
	in_channels = 256;
	out_channels_after_pack = out_channels/CHANNEL_OUT_T;
	out_channels = 128;
	stride = 2;

	////////////////////////////////////////////////
	//////////// layer3_0 PG1 //////////////////////
	LOOP_layer3_0_Conv1_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 12
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_3x3_weight_ptr -= 1;

			bn_relu_bp(
				msb_fmap_tile_buffer_0, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_1,
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3_rot_bp(
				msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, lsb_fmap_tile_buffer,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_3x3_weight_grad_cal
			conv_3x3_grad(
				out_buf_t1[ini - 2*out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_1, grad_buf_t0,
				stride, H_fmap_in
			);
			SGD_WU_3x3(
				grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
		}
	}

	////////////////////////////////////////////////
	//////////// layer3_0 shortcut (conv+bn) ///////
	LOOP_layer3_0_ConvSC_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 11
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_1x1_weight_ptr -= 1;

			load_conv_1x1_weights(
				conv_1x1_weight_tile_buffer, conv_1x1_weight_all[conv_1x1_weight_ptr]
			);
			bn_bp(
				msb_fmap_tile_buffer_0, out_buf_t0[ini], msb_fmap_tile_buffer_1,	// conv+bn shortcut
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			conv_1x1_rot_bp(	// note the index of shortcut input
				msb_fmap_tile_buffer_1, conv_1x1_weight_tile_buffer, msb_fmap_tile_buffer_0,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_1x1_weight_grad_cal
			conv_1x1_grad(
				out_buf_t1[ini - out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_1, grad_buf_t1,
				stride, H_fmap_in
			);
			SGD_WU_1x1(
				grad_buf_t1, conv_1x1_weight_tile_buffer, conv_1x1_weight_all[conv_1x1_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
			shortcut(
				msb_fmap_tile_buffer_0, lsb_fmap_tile_buffer, msb_fmap_tile_buffer_1, out_buf_t0[ini],	// DDR not used
				H_fmap_out, ctrl_sc
			);
		}
	}

	////////////////////////////////////////////////
	//////////// LAYER 2 ///////////////////////////
	////////////////////////////////////////////////

	H_fmap_in = 16;
	H_fmap_out = 16;
	in_channels = 128;
	out_channels_after_pack = out_channels/CHANNEL_OUT_T;
	out_channels = 128;
	stride = 1;

	////////////////////////////////////////////////
	//////////// layer2_1 PG2 //////////////////////
	LOOP_layer2_1_Conv2_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 10
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_3x3_weight_ptr -= 1;

			// lsb_fmap[ini] = msb_fmap[ini];	// identity branch
			identity_shortcut(
				msb_fmap_tile_buffer_1, lsb_fmap_tile_buffer,
				H_fmap_in
			);
			bn_relu_bp(
				msb_fmap_tile_buffer_1, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_0,
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3_rot_bp(
				msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_3x3_weight_grad_cal
			conv_3x3_grad(
				out_buf_t1[ini - out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_0, grad_buf_t0,
				stride, H_fmap_in
			);
			SGD_WU_3x3(
				grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
		}
	}

	////////////////////////////////////////////////
	//////////// layer2_1 PG1 //////////////////////
	LOOP_layer2_1_Conv1_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 9
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_3x3_weight_ptr -= 1;

			bn_relu_bp(
				msb_fmap_tile_buffer_1, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_0,
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3_rot_bp(
				msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_3x3_weight_grad_cal
			conv_3x3_grad(
				out_buf_t1[ini - out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_0, grad_buf_t0,
				stride, H_fmap_in
			);
			SGD_WU_3x3(
				grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
			shortcut(
				msb_fmap_tile_buffer_1, lsb_fmap_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t0[ini],	// DDR not used
				H_fmap_out, ctrl_sc
			);
		}
	}

	////////////////////////////////////////////////
	//////////// layer2_0 PG2 //////////////////////
	LOOP_layer2_0_Conv2_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 8
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_3x3_weight_ptr -= 1;

			bn_relu_bp(
				msb_fmap_tile_buffer_0, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_1,
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3_rot_bp(
				msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_0,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_3x3_weight_grad_cal
			conv_3x3_grad(
				out_buf_t1[ini - out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_1, grad_buf_t0,
				stride, H_fmap_in
			);
			SGD_WU_3x3(
				grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
		}
	}

	////////////////////////////////////////////////
	//////////// LAYER 2 Upsample //////////////////
	////////////////////////////////////////////////

	H_fmap_in = 16;
	H_fmap_out = 32;
	in_channels = 128;
	out_channels_after_pack = out_channels/CHANNEL_OUT_T;
	out_channels = 64;
	stride = 2;

	////////////////////////////////////////////////
	//////////// layer2_0 PG1 //////////////////////
	LOOP_layer2_0_Conv1_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 7
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_3x3_weight_ptr -= 1;

			bn_relu_bp(
				msb_fmap_tile_buffer_0, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_1,
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3_rot_bp(
				msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, lsb_fmap_tile_buffer,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_3x3_weight_grad_cal
			conv_3x3_grad(
				out_buf_t1[ini-1 - 2*out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_1, grad_buf_t0,
				stride, H_fmap_in
			);
			SGD_WU_3x3(
				grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
		}
	}

	////////////////////////////////////////////////
	//////////// layer2_0 shortcut (conv+bn) ///////
	LOOP_layer2_0_ConvSC_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 6
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_1x1_weight_ptr -= 1;

			load_conv_1x1_weights(
				conv_1x1_weight_tile_buffer, conv_1x1_weight_all[conv_1x1_weight_ptr]
			);
			bn_bp(
				msb_fmap_tile_buffer_0, out_buf_t0[ini], msb_fmap_tile_buffer_1,	// conv+bn shortcut
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			conv_1x1_rot_bp(	// note the index of shortcut input
				msb_fmap_tile_buffer_1, conv_1x1_weight_tile_buffer, msb_fmap_tile_buffer_0,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_1x1_weight_grad_cal
			conv_1x1_grad(
				out_buf_t1[ini - out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_1, grad_buf_t1,
				stride, H_fmap_in
			);
			SGD_WU_1x1(
				grad_buf_t1, conv_1x1_weight_tile_buffer, conv_1x1_weight_all[conv_1x1_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
			shortcut(
				msb_fmap_tile_buffer_0, lsb_fmap_tile_buffer, msb_fmap_tile_buffer_1, out_buf_t0[ini],	// DDR not used
				H_fmap_out, ctrl_sc
			);
		}
	}

	////////////////////////////////////////////////
	//////////// LAYER 1 ///////////////////////////
	////////////////////////////////////////////////

	H_fmap_in = 32;
	H_fmap_out = 32;
	in_channels = 64;
	out_channels_after_pack = out_channels/CHANNEL_OUT_T;
	out_channels = 64;
	stride = 1;

	////////////////////////////////////////////////
	//////////// layer1_1 PG2 //////////////////////
	LOOP_layer1_1_Conv2_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 5
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_3x3_weight_ptr -= 1;

			// lsb_fmap[ini] = msb_fmap[ini];	// identity branch
			identity_shortcut(
				msb_fmap_tile_buffer_1, lsb_fmap_tile_buffer,
				H_fmap_in
			);
			bn_relu_bp(
				msb_fmap_tile_buffer_1, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_0,
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3_rot_bp(
				msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_3x3_weight_grad_cal
			conv_3x3_grad(
				out_buf_t1[ini - out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_0, grad_buf_t0,
				stride, H_fmap_in
			);
			SGD_WU_3x3(
				grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
		}
    }

	////////////////////////////////////////////////
	//////////// layer1_1 PG1 //////////////////////
	LOOP_layer1_1_Conv1_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 4
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_3x3_weight_ptr -= 1;

			bn_relu_bp(
				msb_fmap_tile_buffer_1, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_0,
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3_rot_bp(
				msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_3x3_weight_grad_cal
			conv_3x3_grad(
				out_buf_t1[ini - out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_0, grad_buf_t0,
				stride, H_fmap_in
			);
			SGD_WU_3x3(
				grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
			shortcut(
				msb_fmap_tile_buffer_1, lsb_fmap_tile_buffer, msb_fmap_tile_buffer_0, out_buf_t0[ini],	// DDR not used
				H_fmap_out, ctrl_sc
			);
		}
    }

	////////////////////////////////////////////////
	//////////// layer1_0 PG2 //////////////////////
	LOOP_layer1_0_Conv2_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 3
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_3x3_weight_ptr -= 1;

			// lsb_fmap[ini] = msb_fmap[ini];	// identity branch
			identity_shortcut(
				msb_fmap_tile_buffer_0, lsb_fmap_tile_buffer,
				H_fmap_in
			);
			bn_relu_bp(
				msb_fmap_tile_buffer_0, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_1,
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3_rot_bp(
				msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_0,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_3x3_weight_grad_cal
			conv_3x3_grad(
				out_buf_t1[ini - out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_1, grad_buf_t0,
				stride, H_fmap_in
			);
			SGD_WU_3x3(
				grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
		}
    }

	////////////////////////////////////////////////
	//////////// layer1_0 PG1 //////////////////////
	LOOP_layer1_0_Conv1_bp:
	for (int c_out = out_channels/CHANNEL_OUT_T - 1; c_out >= 0; c_out --) {
		ini -= 1;	// ini = 2
		for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
			conv_3x3_weight_ptr -= 1;

			bn_relu_bp(
				msb_fmap_tile_buffer_0, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_1,
				gamma, grad_gamma, grad_beta,
				H_fmap_out
			);
			load_conv_3x3_weights(
				conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			conv_3x3_rot_bp(
				msb_fmap_tile_buffer_1, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_0,
				stride, H_fmap_in, H_fmap_out, c_out, out_channels_after_pack
			);
			///////////////////////////
			// conv_3x3_weight_grad_cal
			conv_3x3_grad(
				out_buf_t1[ini - out_channels/CHANNEL_OUT_T + 1], msb_fmap_tile_buffer_1, grad_buf_t0,
				stride, H_fmap_in
			);
			SGD_WU_3x3(
				grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
			);
			// end gradient calculation
			///////////////////////////
			shortcut(
				msb_fmap_tile_buffer_0, lsb_fmap_tile_buffer, msb_fmap_tile_buffer_1, out_buf_t0[ini],	// DDR not used
				H_fmap_out, ctrl_sc
			);
		}
    }

	////////////////////////////////////////////////
	/////////// Conv 1 + bn 1 + relu 1 /////////////
	////////////////////////////////////////////////

	in_channels = 64;
	out_channels = 3;
	out_channels_after_pack = out_channels/CHANNEL_OUT_T;
	H_fmap_in =32;
	H_fmap_out = 32;
	stride = 1;

    LOOP_Conv1_bp:
	ini -= 1;	// ini = 1
	for (int c_in = in_channels/CHANNEL_IN_T - 1; c_in >=0; c_in --) {
		conv_3x3_weight_ptr -= 1;

		bn_relu_bp(
			msb_fmap_tile_buffer_1, out_buf_t0[ini], relu_mask[ini], msb_fmap_tile_buffer_0,
			gamma, grad_gamma, grad_beta,
			H_fmap_out
		);
		load_conv_3x3_weights(
			conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
		);
		conv_3x3_rot_bp(
			msb_fmap_tile_buffer_0, conv_3x3_weight_tile_buffer, msb_fmap_tile_buffer_1,
			stride, H_fmap_in, H_fmap_out, out_channels_after_pack - 1, out_channels_after_pack
		);
		///////////////////////////
		// conv_3x3_weight_grad_cal
		conv_3x3_grad(
			out_buf_t1[ini - out_channels/CHANNEL_OUT_T], msb_fmap_tile_buffer_0, grad_buf_t0,
			stride, H_fmap_in
		);
		SGD_WU_3x3(
			grad_buf_t0, conv_3x3_weight_tile_buffer, conv_3x3_weight_all[conv_3x3_weight_ptr]
		);
		// end gradient calculation
		///////////////////////////
	}
}	// end FracBNN_T
