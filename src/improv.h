#pragma once
// Improv Wi-Fi serial provisioning (https://www.improv-wifi.com/serial/), used by ESP Web Tools
// so the Wi-Fi network can be chosen in the browser right after flashing.
void improvBegin();
void improvLoop();  // call often from loop()
