#pragma once
#include <Arduino.h>
#include <functional>

// Called after a new layout was saved and validated.
void webBegin(std::function<bool(const String&)> onLayout);
void webLoop();
