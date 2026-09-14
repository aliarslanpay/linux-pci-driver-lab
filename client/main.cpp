// SPDX-License-Identifier: GPL-2.0-only
#include "edu_lab.h"
#include <iostream>
static_assert(sizeof(edu_lab_header) == 16);
static_assert(sizeof(edu_lab_caps) == 80);
static_assert(sizeof(edu_lab_compute) == 32);
static_assert(sizeof(edu_lab_dma) == 4128);
int main() { std::cout << "edu-client: ABI v1 build skeleton\n"; }
