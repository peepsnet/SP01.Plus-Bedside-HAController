#pragma once
// Touchscreen settings overlay: main menu with Wi-Fi and About screens.
void menuInit();
void menuOpen(bool wifiScreen);  // open the overlay (optionally straight on the Wi-Fi screen)
bool menuIsOpen();
