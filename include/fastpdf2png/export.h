// fastpdf2png — DLL export macros
// SPDX-License-Identifier: MIT

#pragma once

#ifdef _WIN32
  #ifdef FASTPDF2PNG_BUILD_DLL
    #define FPDF2PNG_API __declspec(dllexport)
  #else
    #define FPDF2PNG_API __declspec(dllimport)
  #endif
#else
  #define FPDF2PNG_API __attribute__((visibility("default")))
#endif
