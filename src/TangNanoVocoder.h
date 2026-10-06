#pragma once
/**
 * TangNanoVocoder: neural text to speech split between a microcontroller
 * and a Sipeed Tang Nano 20K FPGA. The MCU (ESP32-S3) runs TinyTTS's front
 * end (G2P, text encoder, duration predictor) and sends the latent z_p; the
 * FPGA runs the flow and the vocoder (gateware/) and plays the audio over
 * I2S or a PWM pin. See README.md and docs/.
 *
 * Header only. This header: the link to the FPGA, without TinyTTS:
 *   VocoderClient    the link protocol (status, program upload, sentences)
 *   SPITransport,    the link over SPI or a UART (Arduino)
 *   SerialTransport
 * TangNanoVocoderTTS.h adds TangNanoVocoder: text -> front end -> z_p ->
 * FPGA (needs the TinyTTS library).
 *
 * Data, each in one translation unit only:
 *   "TangNanoVocoder/data/default_vocoder_image.h"    the FPGA's program
 *       image (default_vocoder_image, 1.6MB: flow and vocoder)
 *   "TangNanoVocoder/data/default_frontend_weights.h" the front end's
 *       weights (default_frontend_weights, 0.66MB)
 */
#include "TangNanoVocoder/VocoderClient.h"
#include "TangNanoVocoder/Transports.h"

using namespace tnv;
