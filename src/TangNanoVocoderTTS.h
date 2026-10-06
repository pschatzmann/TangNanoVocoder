#pragma once
/**
 * TangNanoVocoder with TinyTTS: text -> TinyTTS (encoder, duration
 * predictor, flow) on this MCU -> latent -> the vocoder on the Tang Nano 20K.
 * Needs the TinyTTS library (https://github.com/pschatzmann/TinyTTS).
 */
#include "TangNanoVocoder.h"
#include "TinyTTS.h"
#include "TangNanoVocoder/TangNanoVocoder.h"
