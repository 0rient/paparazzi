/*
 * Copyright (C) 2014 Hann Woei Ho
 *               2015 Freek van Tienen <freek.v.tienen@gmail.com>
 *
 * This file is part of Paparazzi.
 *
 * Paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * Paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Paparazzi; see the file COPYING.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

/**
 * @file modules/computer_vision/opticflow/opticflow_calculator.c
 * @brief Estimate velocity from optic flow.
 *
 * Using images from a vertical camera and IMU sensor data.
 */

#include "std.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Own Header
#include "opticflow_calculator.h"

// Computer Vision
#include "lib/vision/image.h"
#include "lib/vision/lucas_kanade.h"
#include "lib/vision/fast_rosten.h"

// ARDrone Vertical Camera Parameters
#define FOV_H 0.67020643276
#define FOV_W 0.89360857702
#define Fx_ARdrone 343.1211
#define Fy_ARdrone 348.5053

// Corner Detection
#define MAX_COUNT 100

// Flow Derotation
#define FLOW_DEROTATION

/* Usefull for calculating FPS */
static uint32_t timeval_diff(struct timeval *starttime, struct timeval *finishtime);

/**
 * Initialize the opticflow calculator
 * @param[out] *opticflow The new optical flow calculator
 * @param[in] *w The image width
 * @param[in] *h The image height
 */
void opticflow_calc_init(struct opticflow_t *opticflow, unsigned int w, unsigned int h)
{
  /* Set the width/height of the image */
  opticflow->img_w = w;
  opticflow->img_h = h;

  /* Create the image buffers */
  opticflow->gray_frame = (unsigned char *) calloc(w * h, sizeof(uint8_t));
  opticflow->prev_gray_frame = (unsigned char *) calloc(w * h, sizeof(uint8_t));

  /* Set the previous values */
  opticflow->got_first_img = FALSE;
  opticflow->prev_pitch = 0.0;
  opticflow->prev_roll = 0.0;
}

/**
 * Run the optical flow on a new image frame
 * @param[in] *opticflow The opticalflow structure that keeps track of previous images
 * @param[in] *state The state of the drone
 * @param[in] *img The image frame to calculate the optical flow from
 * @param[out] *result The optical flow result
 */
void opticflow_calc_frame(struct opticflow_t *opticflow, struct opticflow_state_t *state, struct image_t *img, struct opticflow_result_t *result)
{
  // Corner Tracking
  // Working Variables
  uint16_t borderx = 24, bordery = 24;
  uint16_t x[MAX_COUNT], y[MAX_COUNT];
  uint16_t new_x[MAX_COUNT], new_y[MAX_COUNT];
  bool_t status[MAX_COUNT];
  int dx[MAX_COUNT], dy[MAX_COUNT];

  // Update FPS for information
  result->fps = 1 / (timeval_diff(&opticflow->prev_timestamp, &img->ts) / 1000.);
  memcpy(&opticflow->prev_timestamp, &img->ts, sizeof(struct timeval));

  if (!opticflow->got_first_img) {
    CvtYUYV2Gray(opticflow->prev_gray_frame, img->buf, opticflow->img_w, opticflow->img_h);
    opticflow->got_first_img = TRUE;
  }

  // *************************************************************************************
  // Corner detection
  // *************************************************************************************

  // FAST corner detection
  int fast_threshold = 5;
  xyFAST *pnts_fast = fast9_detect((const byte *)opticflow->prev_gray_frame, opticflow->img_w, opticflow->img_h, opticflow->img_w,
                           fast_threshold, &result->count);
  if (result->count > MAX_COUNT) { result->count = MAX_COUNT; }

  // Copy the points and remove neighboring corners
  const float min_distance2 = 10 * 10;
  bool_t remove_points[MAX_COUNT];
  uint16_t count_fil = 0;
  memset(&remove_points, FALSE, sizeof(bool_t) * MAX_COUNT);

  for (uint16_t i = 0; i < result->count; i++) {
    if(remove_points[i])
      continue;

    x[count_fil] = pnts_fast[i].x;
    y[count_fil++] = pnts_fast[i].y;

    // Skip some if they are too close
    for(uint16_t j = i+1; j < result->count; j++) {
      float distance2 = (x[i] - x[j]) * (x[i] - x[j]) + (y[i] - y[j]) * (y[i] - y[j]);
      if(distance2 < min_distance2)
        remove_points[j] = TRUE;
    }
  }
  free(pnts_fast);
  if(count_fil > 25) count_fil = 25;
  result->count = count_fil;


  uint8_t *im = (uint8_t *)img->buf;
  for(int i = 0; i < result->count; i++) {
    uint16_t idx = 2*y[i]*opticflow->img_w + 2*x[i];
    im[idx] = 255;
    idx = idx+1 % (opticflow->img_w*opticflow->img_h*2);
    im[idx] = 255;
    idx = idx+1 % (opticflow->img_w*opticflow->img_h*2);
    im[idx] = 255;
  }

  // *************************************************************************************
  // Corner Tracking
  // *************************************************************************************
  CvtYUYV2Gray(opticflow->gray_frame, img->buf, opticflow->img_w, opticflow->img_h);

  opticFlowLK(opticflow->gray_frame, opticflow->prev_gray_frame, x, y,
              count_fil, opticflow->img_w, opticflow->img_h, new_x, new_y, status, 5, 100);

  // Remove points if we lost tracking
 /* for (int i = count_fil - 1; i >= 0; i--) {
    if (!status[i] || new_x[i] < borderx || new_x[i] > (opticflow->img_w - 1 - borderx) ||
                       new_y[i] < bordery || new_y[i] > (opticflow->img_h - 1 - bordery)) {
      for (int c = i; c < result->flow_count - 1; c++) {
        x[c] = x[c + 1];
        y[c] = y[c + 1];
        new_x[c] = new_x[c + 1];
        new_y[c] = new_y[c + 1];
      }
      result->flow_count--;
    }
  }*/

  result->dx_sum = 0.0;
  result->dy_sum = 0.0;

  // Optical Flow Computation
  for (int i = 0; i < result->flow_count; i++) {
    dx[i] = new_x[i] - x[i];
    dy[i] = new_y[i] - y[i];
  }

  // Median Filter
  if (result->flow_count) {
    quick_sort_int(dx, result->flow_count); // 11
    quick_sort_int(dy, result->flow_count); // 11

    result->dx_sum = (float) dx[result->flow_count / 2];
    result->dy_sum = (float) dy[result->flow_count / 2];
  } else {
    result->dx_sum = 0.0;
    result->dy_sum = 0.0;
  }

  // Flow Derotation
  /*result->diff_pitch = (state->theta - opticflow->prev_pitch) * opticflow->img_h / FOV_H;
  result->diff_roll = (state->phi - opticflow->prev_roll) * opticflow->img_w / FOV_W;
  opticflow->prev_pitch = state->theta;
  opticflow->prev_roll = state->phi;

  float OFx_trans, OFy_trans;
#ifdef FLOW_DEROTATION
  if (result->flow_count) {
    OFx_trans = result->dx_sum - result->diff_roll;
    OFy_trans = result->dy_sum - result->diff_pitch;

    if ((OFx_trans <= 0) != (result->dx_sum <= 0)) {
      OFx_trans = 0;
      OFy_trans = 0;
    }
  } else {
    OFx_trans = result->dx_sum;
    OFy_trans = result->dy_sum;
  }
#else
  OFx_trans = result->dx_sum;
  OFy_trans = result->dy_sum;
#endif

  // Average Filter
  OFfilter(&result->OFx, &result->OFy, OFx_trans, OFy_trans, result->flow_count, 1);

  // Velocity Computation
  if (state->agl < 0.01) {
    result->cam_h = 0.01;
  }
  else {
    result->cam_h = state->agl;
  }

  if (result->flow_count) {
    result->Velx = result->OFy * result->cam_h * result->fps / Fy_ARdrone + 0.05;
    result->Vely = -result->OFx * result->cam_h * result->fps / Fx_ARdrone - 0.1;
  } else {
    result->Velx = 0.0;
    result->Vely = 0.0;
  }

  // *************************************************************************************
  // Next Loop Preparation
  // *************************************************************************************
  */
  memcpy(opticflow->prev_gray_frame, opticflow->gray_frame, opticflow->img_w * opticflow->img_h);
}

/**
 * Calculate the difference from start till finish
 * @param[in] *starttime The start time to calculate the difference from
 * @param[in] *finishtime The finish time to calculate the difference from
 * @return The difference in milliseconds
 */
static uint32_t timeval_diff(struct timeval *starttime, struct timeval *finishtime)
{
  uint32_t msec;
  msec=(finishtime->tv_sec-starttime->tv_sec)*1000;
  msec+=(finishtime->tv_usec-starttime->tv_usec)/1000;
  return msec;
}