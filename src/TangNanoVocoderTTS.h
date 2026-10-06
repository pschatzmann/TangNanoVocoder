#pragma once
/**
 * TangNanoVocoder with TinyTTS's front end: text -> G2P, text encoder and
 * duration predictor on this MCU -> z_p -> the flow and the vocoder on the
 * Tang Nano 20K. Needs the TinyTTS library
 * (https://github.com/pschatzmann/TinyTTS) for its dictionary data and
 * components; the weights are TangNanoVocoder/data/default_frontend_weights.h.
 */
#include "TangNanoVocoder.h"
#include "TinyTTS.h"
#include "TangNanoVocoder/TangNanoVocoder.h"
