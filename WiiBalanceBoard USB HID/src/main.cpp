#include <Arduino.h>
#include <Keyboard.h>

void setup() {
  // USB HIDキーボードの初期化
  Keyboard.begin();
  
  // UART通信の初期化 (Seeed XIAO: RX=Pin7, TX=Pin6)
  // 送信側(M5Stack)のボーレートに合わせて115200bpsに設定
  Serial1.begin(115200);
}

void loop() {
  // UARTからデータを受信
  if (Serial1.available() > 0) {
    String command = Serial1.readStringUntil('\n');
    command.trim(); // 改行コードや空白を除去

    if (command.length() > 0) {
      // 排他制御（releaseAll）を削除し、CMD:RELEASEが来るまでキーを押し続けるように変更
      // これにより、矢印キーの長押しや、移動しながらのジャンプ（SPACE）などが可能になります

      // コマンドに応じてキーを押す
      if (command == "CMD:UP") {
        Keyboard.press(KEY_UP_ARROW);
      } else if (command == "CMD:DOWN") {
        Keyboard.press(KEY_DOWN_ARROW);
      } else if (command == "CMD:LEFT") {
        Keyboard.press(KEY_LEFT_ARROW);
      } else if (command == "CMD:RIGHT") {
        Keyboard.press(KEY_RIGHT_ARROW);
      } else if (command == "CMD:UP_RIGHT") {
        Keyboard.press(KEY_UP_ARROW);
        Keyboard.press(KEY_RIGHT_ARROW);
      } else if (command == "CMD:UP_LEFT") {
        Keyboard.press(KEY_UP_ARROW);
        Keyboard.press(KEY_LEFT_ARROW);
      } else if (command == "CMD:DOWN_RIGHT") {
        Keyboard.press(KEY_DOWN_ARROW);
        Keyboard.press(KEY_RIGHT_ARROW);
      } else if (command == "CMD:DOWN_LEFT") {
        Keyboard.press(KEY_DOWN_ARROW);
        Keyboard.press(KEY_LEFT_ARROW);
      } else if (command == "CMD:SPACE") {
        Keyboard.write(' ');
      } else if (command == "CMD:RELEASE") {
        Keyboard.releaseAll();
      } 
    }
  }
}