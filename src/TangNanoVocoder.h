#pragma once
/**
 * TangNanoVocoder: neural text to speech split between a microcontroller
 * and a Sipeed Tang Nano 20K FPGA. The MCU (ESP32-S3) runs TinyTTS up to
 * its flow and sends the latent; the FPGA runs the vocoder (gateware/) and
 * plays the audio over I2S or a PWM pin. See README.md and docs/.
 *
 * Header only. This header: the link to the FPGA, without TinyTTS:
 *   VocoderClient    the link protocol (status, program upload, sentences)
 *   SPITransport,    the link over SPI or a UART (Arduino)
 *   SerialTransport
 * TangNanoVocoderTTS.h adds TangNanoVocoder: text -> TinyTTS -> latent ->
 * FPGA (needs the TinyTTS library).
 *
 * The program image the FPGA needs: #include
 * "TangNanoVocoder/data/default_vocoder_image.h" (default_vocoder_image,
 * default_vocoder_image_len) - in one translation unit, it is 484KB.
 */
#include "TangNanoVocoder/VocoderClient.h"
#include "TangNanoVocoder/Transports.h"

using namespace tnv;
