/*
 * engine.c
 * Copyright (C) 2019 Stefan Rehm <droelfdroelf@gmail.com>
 * Copyright (C) 2021 David García Goñi <dagargo@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <endian.h>
#include <time.h>
#include <unistd.h>
#include "engine.h"

#define AUDIO_IN_EP  0x83
#define AUDIO_OUT_EP 0x03
#define MIDI_IN_EP   0x81
#define MIDI_OUT_EP  0x01

#define MIDI_BUF_EVENTS 64
#define MIDI_BUF_SIZE (MIDI_BUF_EVENTS * OB_MIDI_EVENT_SIZE)

#define USB_BULK_MIDI_SIZE 512

#define SAMPLE_TIME_NS (1e9 / ((int)OB_SAMPLE_RATE))

static void prepare_cycle_in_audio ();
static void prepare_cycle_out_audio ();
static void prepare_cycle_in_midi ();

static void
ow_engine_set_name (struct ow_engine *engine, uint8_t bus, uint8_t address)
{
  snprintf (engine->name, OW_LABEL_MAX_LEN, "%s@%03d,%03d",
	    engine->device_desc->name, bus, address);
}

static int
prepare_transfers (struct ow_engine *engine)
{
  engine->usb.xfr_in = libusb_alloc_transfer (0);
  if (!engine->usb.xfr_in)
    {
      return -ENOMEM;
    }

  engine->usb.xfr_out = libusb_alloc_transfer (0);
  if (!engine->usb.xfr_out)
    {
      return -ENOMEM;
    }

  engine->usb.xfr_in_midi = libusb_alloc_transfer (0);
  if (!engine->usb.xfr_in_midi)
    {
      return -ENOMEM;
    }

  engine->usb.xfr_out_midi = libusb_alloc_transfer (0);
  if (!engine->usb.xfr_out_midi)
    {
      return -ENOMEM;
    }

  return LIBUSB_SUCCESS;
}

static void
free_transfers (struct ow_engine *engine)
{
  libusb_free_transfer (engine->usb.xfr_in);
  libusb_free_transfer (engine->usb.xfr_out);
  libusb_free_transfer (engine->usb.xfr_in_midi);
  libusb_free_transfer (engine->usb.xfr_out_midi);
}

inline void
ow_engine_read_usb_input_blocks (struct ow_engine *engine)
{
  int32_t hv;
  int32_t *s;
  struct ow_engine_usb_blk *blk;
  float *f = engine->o2p_transfer_buf;

  for (int i = 0; i < engine->blocks_per_transfer; i++)
    {
      blk = GET_NTH_INPUT_USB_BLK (engine, i);
      s = blk->data;
      for (int j = 0; j < OB_FRAMES_PER_BLOCK; j++)
	{
	  const float *scale = engine->device_desc->output_track_scales;
	  for (int k = 0; k < engine->device_desc->outputs; k++, scale++)
	    {
	      hv = be32toh (*s);
	      *f = hv * (*scale);
	      f++;
	      s++;
	    }
	}
    }
}

static void
set_usb_input_data_blks (struct ow_engine *engine)
{
  size_t wso2p;
  ow_engine_status_t status;

  pthread_spin_lock (&engine->lock);
  if (engine->context->dll)
    {
      ow_dll_overwitch_inc (engine->context->dll, engine->frames_per_transfer,
			    engine->context->get_time ());
    }
  status = engine->status;
  pthread_spin_unlock (&engine->lock);

  ow_engine_read_usb_input_blocks (engine);

  if (status < OW_ENGINE_STATUS_RUN)
    {
      return;
    }

  pthread_spin_lock (&engine->lock);
  engine->o2p_latency =
    engine->context->read_space (engine->context->o2p_audio);
  if (engine->o2p_latency > engine->o2p_max_latency)
    {
      engine->o2p_max_latency = engine->o2p_latency;
    }
  pthread_spin_unlock (&engine->lock);

  wso2p = engine->context->write_space (engine->context->o2p_audio);
  if (engine->o2p_transfer_size <= wso2p)
    {
      engine->context->write (engine->context->o2p_audio,
			      (void *) engine->o2p_transfer_buf,
			      engine->o2p_transfer_size);
    }
  else
    {
      error_print ("o2p: Audio ring buffer overflow. Discarding data...\n");
    }
}

inline void
ow_engine_write_usb_output_blocks (struct ow_engine *engine)
{
  int32_t ov;
  int32_t *s;
  struct ow_engine_usb_blk *blk;
  float *f = engine->p2o_transfer_buf;

  for (int i = 0; i < engine->blocks_per_transfer; i++)
    {
      blk = GET_NTH_OUTPUT_USB_BLK (engine, i);
      blk->frames = htobe16 (engine->usb.frames);
      engine->usb.frames += OB_FRAMES_PER_BLOCK;
      s = blk->data;
      for (int j = 0; j < OB_FRAMES_PER_BLOCK; j++)
	{
	  for (int k = 0; k < engine->device_desc->inputs; k++)
	    {
	      ov = htobe32 ((int32_t) (*f * INT_MAX));
	      *s = ov;
	      f++;
	      s++;
	    }
	}
    }
}

static void
set_usb_output_data_blks (struct ow_engine *engine)
{
  size_t rsp2o;
  size_t bytes;
  long frames;
  int res;
  int p2o_enabled = ow_engine_is_p2o_audio_enabled (engine);

  if (p2o_enabled)
    {
      rsp2o = engine->context->read_space (engine->context->p2o_audio);
      if (!engine->reading_at_p2o_end)
	{
	  if (p2o_enabled && rsp2o >= engine->p2o_transfer_size)
	    {
	      debug_print (2, "p2o: Emptying buffer and running...\n");
	      bytes = ow_bytes_to_frame_bytes (rsp2o, engine->p2o_frame_size);
	      engine->context->read (engine->context->p2o_audio, NULL, bytes);
	      engine->reading_at_p2o_end = 1;
	    }
	  goto set_blocks;
	}
    }
  else
    {
      engine->reading_at_p2o_end = 0;
      debug_print (2, "p2o: Clearing buffer and stopping...\n");
      memset (engine->p2o_transfer_buf, 0, engine->p2o_transfer_size);
      goto set_blocks;
    }

  pthread_spin_lock (&engine->lock);
  engine->p2o_latency = rsp2o;
  if (engine->p2o_latency > engine->p2o_max_latency)
    {
      engine->p2o_max_latency = engine->p2o_latency;
    }
  pthread_spin_unlock (&engine->lock);

  if (rsp2o >= engine->p2o_transfer_size)
    {
      engine->context->read (engine->context->p2o_audio,
			     (void *) engine->p2o_transfer_buf,
			     engine->p2o_transfer_size);
    }
  else
    {
      debug_print (2,
		   "p2o: Audio ring buffer underflow (%zu < %zu). Resampling...\n",
		   rsp2o, engine->p2o_transfer_size);
      frames = rsp2o / engine->p2o_frame_size;
      bytes = frames * engine->p2o_frame_size;
      engine->context->read (engine->context->p2o_audio,
			     (void *) engine->p2o_resampler_buf, bytes);
      engine->p2o_data.input_frames = frames;
      engine->p2o_data.src_ratio =
	(double) engine->frames_per_transfer / frames;
      //We should NOT use the simple API but since this only happens very occasionally and mostly at startup, this has very low impact on audio quality.
      res = src_simple (&engine->p2o_data, SRC_SINC_FASTEST,
			engine->device_desc->inputs);
      if (res)
	{
	  debug_print (2, "p2o: Error while resampling: %s\n",
		       src_strerror (res));
	}
      else if (engine->p2o_data.output_frames_gen !=
	       engine->frames_per_transfer)
	{
	  error_print
	    ("p2o: Unexpected frames with ratio %f (output %ld, expected %d)\n",
	     engine->p2o_data.src_ratio, engine->p2o_data.output_frames_gen,
	     engine->frames_per_transfer);
	}
    }

set_blocks:
  ow_engine_write_usb_output_blocks (engine);
}

static void LIBUSB_CALL
cb_xfr_in (struct libusb_transfer *xfr)
{
  if (xfr->status == LIBUSB_TRANSFER_COMPLETED)
    {
      set_usb_input_data_blks (xfr->user_data);
    }
  else
    {
      error_print ("o2p: Error on USB audio transfer: %s\n",
		   libusb_strerror (xfr->status));
    }
  // start new cycle even if this one did not succeed
  prepare_cycle_in_audio (xfr->user_data);
}

static void LIBUSB_CALL
cb_xfr_out (struct libusb_transfer *xfr)
{
  if (xfr->status != LIBUSB_TRANSFER_COMPLETED)
    {
      error_print ("p2o: Error on USB audio transfer: %s\n",
		   libusb_strerror (xfr->status));
    }
  set_usb_output_data_blks (xfr->user_data);
  // We have to make sure that the out cycle is always started after its callback
  // Race condition on slower systems!
  prepare_cycle_out_audio (xfr->user_data);
}

static void LIBUSB_CALL
cb_xfr_in_midi (struct libusb_transfer *xfr)
{
  struct ow_midi_event event;
  int length;
  struct ow_engine *engine = xfr->user_data;

  if (ow_engine_get_status (engine) < OW_ENGINE_STATUS_RUN)
    {
      goto end;
    }

  if (xfr->status == LIBUSB_TRANSFER_COMPLETED)
    {
      length = 0;
      event.time = engine->context->get_time ();

      while (length < xfr->actual_length)
	{
	  memcpy (event.bytes, &engine->o2p_midi_data[length],
		  OB_MIDI_EVENT_SIZE);
	  //Note-off, Note-on, Poly-KeyPress, Control Change, Program Change, Channel Pressure, PitchBend Change, Single Byte
	  if (event.bytes[0] >= 0x08 && event.bytes[0] <= 0x0f)
	    {
	      debug_print (2, "o2p MIDI: %02x, %02x, %02x, %02x (%f)\n",
			   event.bytes[0], event.bytes[1], event.bytes[2],
			   event.bytes[3], event.time);

	      if (engine->context->write_space (engine->context->o2p_midi) >=
		  sizeof (struct ow_midi_event))
		{
		  engine->context->write (engine->context->o2p_midi,
					  (void *) &event,
					  sizeof (struct ow_midi_event));
		}
	      else
		{
		  error_print
		    ("o2p: MIDI ring buffer overflow. Discarding data...\n");
		}
	    }
	  length += OB_MIDI_EVENT_SIZE;
	}
    }
  else
    {
      if (xfr->status != LIBUSB_TRANSFER_TIMED_OUT)
	{
	  error_print ("Error on USB MIDI in transfer: %s\n",
		       libusb_strerror (xfr->status));
	}
    }

end:
  prepare_cycle_in_midi (engine);
}

static void LIBUSB_CALL
cb_xfr_out_midi (struct libusb_transfer *xfr)
{
  struct ow_engine *engine = xfr->user_data;

  pthread_spin_lock (&engine->p2o_midi_lock);
  engine->p2o_midi_ready = 1;
  pthread_spin_unlock (&engine->p2o_midi_lock);

  if (xfr->status != LIBUSB_TRANSFER_COMPLETED)
    {
      error_print ("Error on USB MIDI out transfer: %s\n",
		   libusb_strerror (xfr->status));
    }
}

static void
prepare_cycle_out_audio (struct ow_engine *engine)
{
  libusb_fill_interrupt_transfer (engine->usb.xfr_out,
				  engine->usb.device_handle, AUDIO_OUT_EP,
				  (void *) engine->usb.data_out,
				  engine->usb.data_out_len, cb_xfr_out,
				  engine, 0);

  int err = libusb_submit_transfer (engine->usb.xfr_out);
  if (err)
    {
      error_print ("p2o: Error when submitting USB audio transfer: %s\n",
		   libusb_strerror (err));
      ow_engine_set_status (engine, OW_ENGINE_STATUS_ERROR);
    }
}

static void
prepare_cycle_in_audio (struct ow_engine *engine)
{
  libusb_fill_interrupt_transfer (engine->usb.xfr_in,
				  engine->usb.device_handle, AUDIO_IN_EP,
				  (void *) engine->usb.data_in,
				  engine->usb.data_in_len, cb_xfr_in, engine,
				  0);

  int err = libusb_submit_transfer (engine->usb.xfr_in);
  if (err)
    {
      error_print ("o2p: Error when submitting USB audio in transfer: %s\n",
		   libusb_strerror (err));
      ow_engine_set_status (engine, OW_ENGINE_STATUS_ERROR);
    }
}

static void
prepare_cycle_in_midi (struct ow_engine *engine)
{
  libusb_fill_bulk_transfer (engine->usb.xfr_in_midi,
			     engine->usb.device_handle, MIDI_IN_EP,
			     (void *) engine->o2p_midi_data,
			     USB_BULK_MIDI_SIZE, cb_xfr_in_midi, engine, 0);

  int err = libusb_submit_transfer (engine->usb.xfr_in_midi);
  if (err)
    {
      error_print ("o2p: Error when submitting USB MIDI transfer: %s\n",
		   libusb_strerror (err));
      ow_engine_set_status (engine, OW_ENGINE_STATUS_ERROR);
    }
}

static void
prepare_cycle_out_midi (struct ow_engine *engine)
{
  libusb_fill_bulk_transfer (engine->usb.xfr_out_midi,
			     engine->usb.device_handle, MIDI_OUT_EP,
			     (void *) engine->p2o_midi_data,
			     USB_BULK_MIDI_SIZE, cb_xfr_out_midi, engine, 0);

  int err = libusb_submit_transfer (engine->usb.xfr_out_midi);
  if (err)
    {
      error_print ("p2o: Error when submitting USB MIDI transfer: %s\n",
		   libusb_strerror (err));
      ow_engine_set_status (engine, OW_ENGINE_STATUS_ERROR);
    }
}

static void
usb_shutdown (struct ow_engine *engine)
{
  libusb_close (engine->usb.device_handle);
  libusb_exit (engine->usb.context);
}

void
ow_engine_init_mem (struct ow_engine *engine, int blocks_per_transfer)
{
  struct ow_engine_usb_blk *blk;

  pthread_spin_init (&engine->lock, PTHREAD_PROCESS_SHARED);

  engine->blocks_per_transfer = blocks_per_transfer;
  engine->frames_per_transfer =
    OB_FRAMES_PER_BLOCK * engine->blocks_per_transfer;

  engine->usb.data_in_blk_len =
    sizeof (struct ow_engine_usb_blk) +
    sizeof (int32_t) * OB_FRAMES_PER_BLOCK * engine->device_desc->outputs;
  engine->usb.data_out_blk_len =
    sizeof (struct ow_engine_usb_blk) +
    sizeof (int32_t) * OB_FRAMES_PER_BLOCK * engine->device_desc->inputs;

  engine->usb.frames = 0;
  engine->usb.data_in_len =
    engine->usb.data_in_blk_len * engine->blocks_per_transfer;
  engine->usb.data_out_len =
    engine->usb.data_out_blk_len * engine->blocks_per_transfer;
  engine->usb.data_in = malloc (engine->usb.data_in_len);
  engine->usb.data_out = malloc (engine->usb.data_out_len);
  memset (engine->usb.data_in, 0, engine->usb.data_in_len);
  memset (engine->usb.data_out, 0, engine->usb.data_out_len);

  for (int i = 0; i < engine->blocks_per_transfer; i++)
    {
      blk = GET_NTH_OUTPUT_USB_BLK (engine, i);
      blk->header = htobe16 (0x07ff);
    }

  engine->p2o_frame_size = OB_BYTES_PER_SAMPLE * engine->device_desc->inputs;
  engine->o2p_frame_size = OB_BYTES_PER_SAMPLE * engine->device_desc->outputs;

  engine->p2o_transfer_size =
    engine->frames_per_transfer * engine->p2o_frame_size;
  engine->o2p_transfer_size =
    engine->frames_per_transfer * engine->o2p_frame_size;
  engine->p2o_transfer_buf = malloc (engine->p2o_transfer_size);
  engine->o2p_transfer_buf = malloc (engine->o2p_transfer_size);
  memset (engine->p2o_transfer_buf, 0, engine->p2o_transfer_size);
  memset (engine->o2p_transfer_buf, 0, engine->o2p_transfer_size);

  //o2p resampler
  engine->p2o_resampler_buf = malloc (engine->p2o_transfer_size);
  memset (engine->p2o_resampler_buf, 0, engine->p2o_transfer_size);
  engine->p2o_data.data_in = engine->p2o_resampler_buf;
  engine->p2o_data.data_out = engine->p2o_transfer_buf;
  engine->p2o_data.end_of_input = 1;
  engine->p2o_data.input_frames = engine->frames_per_transfer;
  engine->p2o_data.output_frames = engine->frames_per_transfer;

  //MIDI
  engine->p2o_midi_data = malloc (USB_BULK_MIDI_SIZE);
  engine->o2p_midi_data = malloc (USB_BULK_MIDI_SIZE);
  memset (engine->p2o_midi_data, 0, USB_BULK_MIDI_SIZE);
  memset (engine->o2p_midi_data, 0, USB_BULK_MIDI_SIZE);
  pthread_spin_init (&engine->p2o_midi_lock, PTHREAD_PROCESS_SHARED);
}

// initialization taken from sniffed session

static ow_err_t
ow_engine_init (struct ow_engine *engine, int blocks_per_transfer)
{
  int err;
  ow_err_t ret = OW_OK;

  err = libusb_set_configuration (engine->usb.device_handle, 1);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_SET_USB_CONFIG;
      goto end;
    }
  err = libusb_claim_interface (engine->usb.device_handle, 1);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_CLAIM_IF;
      goto end;
    }
  err = libusb_set_interface_alt_setting (engine->usb.device_handle, 1, 3);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_SET_ALT_SETTING;
      goto end;
    }
  err = libusb_claim_interface (engine->usb.device_handle, 2);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_CLAIM_IF;
      goto end;
    }
  err = libusb_set_interface_alt_setting (engine->usb.device_handle, 2, 2);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_SET_ALT_SETTING;
      goto end;
    }
  err = libusb_claim_interface (engine->usb.device_handle, 3);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_CLAIM_IF;
      goto end;
    }
  err = libusb_set_interface_alt_setting (engine->usb.device_handle, 3, 0);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_SET_ALT_SETTING;
      goto end;
    }
  err = libusb_clear_halt (engine->usb.device_handle, AUDIO_IN_EP);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_CLEAR_EP;
      goto end;
    }
  err = libusb_clear_halt (engine->usb.device_handle, AUDIO_OUT_EP);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_CLEAR_EP;
      goto end;
    }
  err = libusb_clear_halt (engine->usb.device_handle, MIDI_IN_EP);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_CLEAR_EP;
      goto end;
    }
  err = libusb_clear_halt (engine->usb.device_handle, MIDI_OUT_EP);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_CLEAR_EP;
      goto end;
    }
  err = prepare_transfers (engine);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_PREPARE_TRANSFER;
    }

end:
  if (ret == OW_OK)
    {
      ow_engine_init_mem (engine, blocks_per_transfer);
    }
  else
    {
      usb_shutdown (engine);
      free (engine);
      error_print ("Error while initializing device: %s\n",
		   libusb_error_name (ret));
    }
  return ret;
}

ow_err_t
ow_engine_init_from_libusb_device_descriptor (struct ow_engine **engine_,
					      int libusb_device_descriptor,
					      int blocks_per_transfer)
{
  ow_err_t err;
  uint8_t bus, address;
  struct ow_engine *engine;
  struct libusb_device *device;
  struct libusb_device_descriptor desc;

  if (libusb_set_option (NULL, LIBUSB_OPTION_WEAK_AUTHORITY, NULL) !=
      LIBUSB_SUCCESS)
    {
      return OW_USB_ERROR_LIBUSB_INIT_FAILED;
    }

  engine = malloc (sizeof (struct ow_engine));

  if (libusb_init (&engine->usb.context) != LIBUSB_SUCCESS)
    {
      err = OW_USB_ERROR_LIBUSB_INIT_FAILED;
      goto error;
    }

  if (libusb_wrap_sys_device (NULL, (intptr_t) libusb_device_descriptor,
			      &engine->usb.device_handle))
    {
      err = OW_USB_ERROR_LIBUSB_INIT_FAILED;
      goto error;
    }

  device = libusb_get_device (engine->usb.device_handle);
  libusb_get_device_descriptor (device, &desc);
  ow_get_device_desc_from_vid_pid (desc.idVendor, desc.idProduct,
				   &engine->device_desc);

  *engine_ = engine;
  err = ow_engine_init (engine, blocks_per_transfer);
  if (!err)
    {
      bus = libusb_get_bus_number (device);
      address = libusb_get_device_address (device);
      ow_engine_set_name (engine, bus, address);
      return err;
    }

error:
  free (engine);
  return err;
}

ow_err_t
ow_engine_init_from_bus_address (struct ow_engine **engine_,
				 uint8_t bus, uint8_t address,
				 int blocks_per_transfer)
{
  int err;
  ow_err_t ret;
  ssize_t total = 0;
  libusb_device **devices;
  libusb_device **device;
  struct ow_engine *engine;
  struct libusb_device_descriptor desc;

  engine = malloc (sizeof (struct ow_engine));

  if (libusb_init (&engine->usb.context) != LIBUSB_SUCCESS)
    {
      ret = OW_USB_ERROR_LIBUSB_INIT_FAILED;
      goto error;
    }

  engine->usb.device_handle = NULL;
  total = libusb_get_device_list (engine->usb.context, &devices);
  device = devices;
  for (int i = 0; i < total; i++, device++)
    {
      err = libusb_get_device_descriptor (*device, &desc);
      if (err)
	{
	  error_print ("Error while getting device description: %s",
		       libusb_error_name (err));
	  continue;
	}

      if (ow_get_device_desc_from_vid_pid
	  (desc.idVendor, desc.idProduct, &engine->device_desc)
	  && libusb_get_bus_number (*device) == bus
	  && libusb_get_device_address (*device) == address)
	{
	  if (libusb_open (*device, &engine->usb.device_handle))
	    {
	      error_print ("Error while opening device: %s\n",
			   libusb_error_name (err));
	    }
	  else
	    {
	      ow_engine_set_name (engine, bus, address);
	    }

	  break;
	}
    }

  libusb_free_device_list (devices, total);

  if (!engine->usb.device_handle)
    {
      ret = OW_USB_ERROR_CANT_FIND_DEV;
      goto error;
    }

  *engine_ = engine;
  return ow_engine_init (engine, blocks_per_transfer);

error:
  free (engine);
  return ret;
}

static const char *ob_err_strgs[] = {
  "ok",
  "generic error",
  "libusb init failed",
  "can't open device",
  "can't set usb config",
  "can't claim usb interface",
  "can't set usb alt setting",
  "can't cleat endpoint",
  "can't prepare transfer",
  "can't find a matching device",
  "'read_space' not set in context",
  "'write_space' not set in context",
  "'read' not set in context",
  "'write' not set in context",
  "'p2o_audio_buf' not set in context",
  "'o2p_audio_buf' not set in context",
  "'p2o_midi_buf' not set in context",
  "'o2p_midi_buf' not set in context",
  "'get_time' not set in context",
  "'dll' not set in context"
};

static void *
run_p2o_midi (void *data)
{
  int pos, p2o_midi_ready, event_read = 0;
  double last_time, diff;
  struct timespec sleep_time, smallest_sleep_time;
  struct ow_midi_event event;
  struct ow_engine *engine = data;

  smallest_sleep_time.tv_sec = 0;
  smallest_sleep_time.tv_nsec = SAMPLE_TIME_NS * 32 / 2;	//Average wait time for a 32 buffer sample

  pos = 0;
  diff = 0.0;
  last_time = engine->context->get_time ();
  engine->p2o_midi_ready = 1;
  while (1)
    {

      while (engine->context->read_space (engine->context->p2o_midi) >=
	     sizeof (struct ow_midi_event) && pos < USB_BULK_MIDI_SIZE)
	{
	  if (!pos)
	    {
	      memset (engine->p2o_midi_data, 0, USB_BULK_MIDI_SIZE);
	      diff = 0;
	    }

	  if (!event_read)
	    {
	      engine->context->read (engine->context->p2o_midi,
				     (void *) &event,
				     sizeof (struct ow_midi_event));
	      event_read = 1;
	    }

	  if (event.time > last_time)
	    {
	      diff = event.time - last_time;
	      last_time = event.time;
	      break;
	    }

	  memcpy (&engine->p2o_midi_data[pos], event.bytes,
		  OB_MIDI_EVENT_SIZE);
	  pos += OB_MIDI_EVENT_SIZE;
	  event_read = 0;
	}

      if (pos)
	{
	  debug_print (2, "Event frames: %f; diff: %f\n", event.time, diff);
	  engine->p2o_midi_ready = 0;
	  prepare_cycle_out_midi (engine);
	  pos = 0;
	}

      if (diff)
	{
	  sleep_time.tv_sec = diff;
	  sleep_time.tv_nsec = (diff - sleep_time.tv_sec) * 1.0e9;
	  nanosleep (&sleep_time, NULL);
	}
      else
	{
	  nanosleep (&smallest_sleep_time, NULL);
	}

      pthread_spin_lock (&engine->p2o_midi_lock);
      p2o_midi_ready = engine->p2o_midi_ready;
      pthread_spin_unlock (&engine->p2o_midi_lock);
      while (!p2o_midi_ready)
	{
	  nanosleep (&smallest_sleep_time, NULL);
	  pthread_spin_lock (&engine->p2o_midi_lock);
	  p2o_midi_ready = engine->p2o_midi_ready;
	  pthread_spin_unlock (&engine->p2o_midi_lock);
	};

      if (ow_engine_get_status (engine) <= OW_ENGINE_STATUS_STOP)
	{
	  break;
	}
    }

  return NULL;
}

static void *
run_audio_o2p_midi (void *data)
{
  size_t rsp2o, bytes;
  struct ow_engine *engine = data;

  while (ow_engine_get_status (engine) == OW_ENGINE_STATUS_READY);

  //status == OW_ENGINE_STATUS_BOOT

  prepare_cycle_in_audio (engine);
  prepare_cycle_out_audio (engine);
  if (engine->options.o2p_midi)
    {
      prepare_cycle_in_midi (engine);
    }

  while (1)
    {
      engine->p2o_latency = 0;
      engine->p2o_max_latency = 0;
      engine->reading_at_p2o_end = 0;
      engine->o2p_latency = 0;
      engine->o2p_max_latency = 0;

      //status == OW_ENGINE_STATUS_BOOT

      pthread_spin_lock (&engine->lock);
      if (engine->context->dll)
	{
	  ow_dll_overwitch_init (engine->context->dll, OB_SAMPLE_RATE,
				 engine->frames_per_transfer,
				 engine->context->get_time ());
	  engine->status = OW_ENGINE_STATUS_WAIT;
	}
      else
	{
	  engine->status = OW_ENGINE_STATUS_RUN;
	}
      pthread_spin_unlock (&engine->lock);

      while (ow_engine_get_status (engine) >= OW_ENGINE_STATUS_WAIT)
	{
	  libusb_handle_events_completed (engine->usb.context, NULL);
	}

      if (ow_engine_get_status (engine) <= OW_ENGINE_STATUS_STOP)
	{
	  break;
	}

      rsp2o = engine->context->read_space (engine->context->p2o_audio);
      bytes = ow_bytes_to_frame_bytes (rsp2o, engine->p2o_frame_size);
      engine->context->read (engine->context->p2o_audio, NULL, bytes);
      memset (engine->p2o_transfer_buf, 0, engine->p2o_transfer_size);
    }

  return NULL;
}

ow_err_t
ow_engine_activate (struct ow_engine *engine, struct ow_context *context)
{
  engine->context = context;

  if (!context->options)
    {
      return OW_GENERIC_ERROR;
    }

  engine->options.o2p_audio = context->options & OW_ENGINE_OPTION_O2P_AUDIO;
  if (engine->options.o2p_audio)
    {
      if (!context->write_space)
	{
	  return OW_INIT_ERROR_NO_WRITE_SPACE;
	}
      if (!context->write)
	{
	  return OW_INIT_ERROR_NO_WRITE;
	}
      if (!context->o2p_audio)
	{
	  return OW_INIT_ERROR_NO_O2P_AUDIO_BUF;
	}
    }

  engine->options.p2o_audio = context->options & OW_ENGINE_OPTION_P2O_AUDIO;
  if (engine->options.p2o_audio)
    {
      if (!context->read_space)
	{
	  return OW_INIT_ERROR_NO_READ_SPACE;
	}
      if (!context->read)
	{
	  return OW_INIT_ERROR_NO_READ;
	}
      if (!context->p2o_audio)
	{
	  return OW_INIT_ERROR_NO_P2O_AUDIO_BUF;
	}
    }

  engine->options.o2p_midi = context->options & OW_ENGINE_OPTION_O2P_MIDI;
  if (engine->options.o2p_midi)
    {
      if (!context->get_time)
	{
	  return OW_INIT_ERROR_NO_GET_TIME;
	}
      if (!context->o2p_midi)
	{
	  return OW_INIT_ERROR_NO_O2P_MIDI_BUF;
	}
    }

  engine->options.p2o_midi = context->options & OW_ENGINE_OPTION_P2O_MIDI;
  if (engine->options.p2o_midi)
    {
      if (!context->get_time)
	{
	  return OW_INIT_ERROR_NO_GET_TIME;
	}
      if (!context->p2o_midi)
	{
	  return OW_INIT_ERROR_NO_P2O_MIDI_BUF;
	}
    }

  engine->options.dll = context->options & OW_ENGINE_OPTION_DLL;
  if (engine->options.dll)
    {
      if (!context->get_time)
	{
	  return OW_INIT_ERROR_NO_GET_TIME;
	}
      if (!context->dll)
	{
	  return OW_INIT_ERROR_NO_DLL;
	}
      engine->status = OW_ENGINE_STATUS_READY;
    }

  if (!context->set_rt_priority)
    {
      context->set_rt_priority = ow_set_thread_rt_priority;
      context->priority = OW_DEFAULT_RT_PROPERTY;
    }

  if (engine->options.p2o_midi)
    {
      debug_print (1, "Starting p2o MIDI thread...\n");
      if (pthread_create (&engine->p2o_midi_thread, NULL, run_p2o_midi,
			  engine))
	{
	  error_print ("Could not start MIDI thread\n");
	  return OW_GENERIC_ERROR;
	}
      context->set_rt_priority (&engine->p2o_midi_thread,
				engine->context->priority);
    }

  if (engine->options.o2p_midi || engine->options.o2p_audio
      || engine->options.p2o_audio)
    {
      debug_print (1, "Starting audio and o2p MIDI thread...\n");
      if (pthread_create (&engine->audio_o2p_midi_thread, NULL,
			  run_audio_o2p_midi, engine))
	{
	  error_print ("Could not start device thread\n");
	  return OW_GENERIC_ERROR;
	}
      context->set_rt_priority (&engine->audio_o2p_midi_thread,
				engine->context->priority);
    }


  return OW_OK;
}

inline void
ow_engine_wait (struct ow_engine *engine)
{
  pthread_join (engine->audio_o2p_midi_thread, NULL);
  if (engine->options.o2p_midi)
    {
      pthread_join (engine->p2o_midi_thread, NULL);
    }
}

const char *
ow_get_err_str (ow_err_t errcode)
{
  return ob_err_strgs[errcode];
}

void
ow_engine_destroy (struct ow_engine *engine)
{
  usb_shutdown (engine);
  free_transfers (engine);
  ow_engine_free_mem (engine);
  free (engine);
}

void
ow_engine_free_mem (struct ow_engine *engine)
{
  free (engine->p2o_transfer_buf);
  free (engine->p2o_resampler_buf);
  free (engine->o2p_transfer_buf);
  free (engine->usb.data_in);
  free (engine->usb.data_out);
  free (engine->p2o_midi_data);
  free (engine->o2p_midi_data);
  pthread_spin_destroy (&engine->lock);
  pthread_spin_destroy (&engine->p2o_midi_lock);
}

inline ow_engine_status_t
ow_engine_get_status (struct ow_engine *engine)
{
  ow_engine_status_t status;
  pthread_spin_lock (&engine->lock);
  status = engine->status;
  pthread_spin_unlock (&engine->lock);
  return status;
}

inline void
ow_engine_set_status (struct ow_engine *engine, ow_engine_status_t status)
{
  pthread_spin_lock (&engine->lock);
  engine->status = status;
  pthread_spin_unlock (&engine->lock);
}

inline int
ow_engine_is_p2o_audio_enabled (struct ow_engine *engine)
{
  int enabled;
  pthread_spin_lock (&engine->lock);
  enabled = engine->options.p2o_audio;
  pthread_spin_unlock (&engine->lock);
  return enabled;
}

inline void
ow_engine_set_p2o_audio_enabled (struct ow_engine *engine, int enabled)
{
  int last = ow_engine_is_p2o_audio_enabled (engine);
  if (last != enabled)
    {
      pthread_spin_lock (&engine->lock);
      engine->options.p2o_audio = enabled;
      pthread_spin_unlock (&engine->lock);
      debug_print (1, "Setting p2o audio to %d...\n", enabled);
    }
}

inline int
ow_bytes_to_frame_bytes (int bytes, int bytes_per_frame)
{
  int frames = bytes / bytes_per_frame;
  return frames * bytes_per_frame;
}

const struct ow_device_desc *
ow_engine_get_device_desc (struct ow_engine *engine)
{
  return engine->device_desc;
}

inline void
ow_engine_stop (struct ow_engine *engine)
{
  ow_engine_set_status (engine, OW_ENGINE_STATUS_STOP);
}

void
ow_engine_print_blocks (struct ow_engine *engine, char *blks, size_t blk_len)
{
  int32_t *s, v;
  struct ow_engine_usb_blk *blk;

  for (int i = 0; i < engine->blocks_per_transfer; i++)
    {
      blk = GET_NTH_USB_BLK (blks, blk_len, i);
      printf ("Block %d\n", i);
      printf ("0x%04x | 0x%04x\n", be16toh (blk->header),
	      be16toh (blk->frames));
      s = blk->data;
      for (int j = 0; j < OB_FRAMES_PER_BLOCK; j++)
	{
	  const float *scale = engine->device_desc->output_track_scales;
	  for (int k = 0; k < engine->device_desc->outputs; k++, scale++)
	    {
	      v = be32toh (*s);
	      printf ("Frame %2d, track %2d: %d\n", j, k, v);
	      s++;
	    }
	}
    }
}
