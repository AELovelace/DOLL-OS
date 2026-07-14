//   DOLL-OS.ino
//   entry point, wires up hardware init and drives the main loop
//   no vibecode. section is more-or-less complete. 
#include <M5Cardputer.h>
#include <WiFi.h>
#include <FS.h>
#include "config.h"
#include "global.h"

 void setup() {
  //config cardputer. auto determines type
  auto cfg = M5.config();
  //Start cardputer hardware. True == kb on
  M5Cardputer.begin(cfg, true);

  //init screen
  M5Cardputer.Display.fillScreen(BLACK);      //clear to black on boot
  M5Cardputer.Display.setTextSize(1);         //base text size for the display
  M5Cardputer.Display.setTextColor(WHITE);    //base text color for the display

  //status bar sprite, sits at the very top of the screen
  statusBarSprite.setColorDepth(16);          //16-bit color to keep sprite memory reasonable
  statusBarSprite.createSprite(M5Cardputer.Display.width(), STATUS_BAR_HEIGHT);   //full width, fixed status bar height

  //terminal sprite, fills the space between the status bar and command bar
  terminalSprite.setColorDepth(16);
  terminalSprite.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height() - STATUS_BAR_HEIGHT - COMMAND_BAR_HEIGHT);   //remaining vertical space
  terminalSprite.setTextColor(WHITE,BLACK);   //white text on black background

  //command bar sprite, sits at the very bottom of the screen
  commandBarSprite.setColorDepth(16);
  commandBarSprite.createSprite(M5Cardputer.Display.width(), COMMAND_BAR_HEIGHT);   //full width, fixed command bar height
  commandBarSprite.setTextColor(WHITE,BLACK);

  //display bootSplash
    drawBootLogo();
    delay(1000);
  //mount internal flash (LittleFS) and the SD card
  initStorage();
}

void loop() {
  // update current state of m5 library
  M5Cardputer.update();             //update cardputer status
  statusManagement();               //run statusManagement adn construct system stats bar 
  drawTerminalHistory();            //draw term history
  drawCommandBar(currentCommand);   //draw text input area
  keyboardLogic();                  //handle keyboard logic

}
