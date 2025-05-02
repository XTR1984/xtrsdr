#include <Arduino.h>
//#include "USB.h"
#include "libusb.h"
#include <rtl-sdr.h>
#include "convenience.h"
#include "esp32-hal-cpu.h"
#define TAG "main"
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"


#define LED 15
#define SAMPLE_RATE 240000
#define FREQUENCY 102400000
#define PPM_ERROR 0


int direct_sampling = 0;
int gain = 100;
uint32_t frequency = FREQUENCY;
uint32_t samp_rate = SAMPLE_RATE;

const int buttonPin = 0;         // input pin for pushbutton
int previousButtonState = HIGH;  // for checking the state of a pushButton
int counter = 0;                 // button push counter
int cb_counter=0;
int do_exit = 0;
int bytes_to_read = 1024001;

rtlsdr_dev_t *dev = NULL;

void processCommand(String cmd);

void SerialConsole() {
  static String inputBuffer;
  Serial0.println("Console:");
  while (1) {
    delay(10);
    if (!Serial0.available()) continue;
    char c = Serial0.read();
    Serial0.print(c);
    if (c == '\n' || c == '\r') {
      if (inputBuffer.length() > 0) {
        processCommand(inputBuffer);
        inputBuffer = "";
      }
    } else {
      inputBuffer += c;
    }
  }
}

String getValue(String data, char separator, int index)  //by Odis Harkins https://arduino.stackexchange.com/a/1237
{
    int found = 0;
    int strIndex[] = { 0, -1 };
    int maxIndex = data.length() - 1;

    for (int i = 0; i <= maxIndex && found <= index; i++) {
        if (data.charAt(i) == separator || i == maxIndex) {
            found++;
            strIndex[0] = strIndex[1] + 1;
            strIndex[1] = (i == maxIndex) ? i+1 : i;
        }
    }
    return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

void PrintValue(int value){
        Serial0.print(value, DEC);
        Serial0.print("\t");
         
        if (value < 0x10) Serial.print("0");
        Serial0.print(value, HEX);
        Serial0.print("h");
        Serial0.print("\t");
          
        for (int b = 16; b >= 0; b--) {
          Serial0.print((value >> b) & 1 ? "1" : "0");
          if (b%4==0) Serial.print(" ");
        }

  }

int inthex(String Str){
  if (Str.startsWith("0X")){ 
      Str = Str.substring(2);
      return strtoul(Str.c_str(), NULL, 16);
  }
  else {
      return Str.toInt();
  }
}

void processCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();
  
  if (cmd.startsWith("R,")) {
    int block = 0;
    uint32_t address = 0;
    int len = 0;

    block = getValue(cmd,',',1).toInt();
    address = inthex( getValue(cmd,',',2));
    len = getValue(cmd,',',3).toInt();
    
    if (len > 0) {
        Serial0.printf("Read block %d, address %d, len %d    ",block,address,len);
        uint16_t value = rtlsdr_read_reg(dev,(uint8_t) block, (uint16_t) address, (uint8_t) len);
        
        Serial0.println();
        PrintValue(value);

    } else {
      Serial0.println("Invalid R command format. Use: R,Block,Address,Len");
      Serial0.println("Where: Block and Len are decimal, Address is hex or decimal");
    }  
  } else if (cmd.startsWith("RI,")){
    uint32_t address = 0;
    int len = 0;
      
    address = inthex(getValue(cmd,',',1));
    int reg = inthex(getValue(cmd,',',2));
    
    Serial0.printf("Read i2c address %d, reg %d    ",address,reg);
    uint16_t value = rtlsdr_i2c_read_reg(dev,(uint16_t) address, (uint8_t) reg);
    Serial0.println();
    PrintValue(value);

  } else if (cmd.startsWith("RP+")){
      rtlsdr_set_i2c_repeater(dev, 1);
  } else if (cmd.startsWith("RP-")){  
      rtlsdr_set_i2c_repeater(dev, 0);
  }

  else
  {
       Serial0.println("Unknown command. Supported: R,Block,Address,Len");
   }
}


void printMemoryInfo() {
  Serial0.println("\nMemory Information:");
  // common info
  Serial0.printf("Total heap: %d bytes\n", ESP.getHeapSize());
  Serial0.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  Serial0.printf("Minimum free heap: %d bytes\n", ESP.getMinFreeHeap());
  // PSRAM info
  Serial0.printf("Total PSRAM: %d bytes\n", ESP.getPsramSize());
  Serial0.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
}


static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
  cb_counter++;
  digitalWrite(LED,!digitalRead(LED));
  //if (cb_counter%5==0) {
    Serial0.printf("cb_counter = %d len=%d toread=%d\n",cb_counter, len,bytes_to_read);
  //}

  //if (ctx) {
		if (do_exit)
			return;

		if ((bytes_to_read > 0) && (bytes_to_read < len)) {
			len = bytes_to_read;
			do_exit = 1;
			rtlsdr_cancel_async(dev);
		}
    ESP_LOGD(TAG, "Start work");
    int sum =0;
    for (int i =1; i<len; i+=2){
      sum += sqrt(buf[i]*buf[i]+buf[i+1]*buf[i+1]);
    }
    if (len!=0)  Serial0.printf("avgsum=%d\n", sum/len);
    ESP_LOGD(TAG, "End work");

    int freeram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGD(TAG, "free ram %d bytes", freeram);


		//if (fwrite(buf, 1, len, (FILE*)ctx) != len) {
		//	fprintf(stderr, "Short write, samples lost, exiting!\n");
    //	rtlsdr_cancel_async(dev);
		//}

		if (bytes_to_read > 0) 	bytes_to_read -= len;
	//}
}


void setup() {
  pinMode(LED, OUTPUT);
  Serial0.begin(115200,SERIAL_8N1,18,17);
  //while(!Serial0) delay(100);
  delay(10);
  //esp_log_set_vprintf(my_log_vprintf);
  //Serial0.println("setup start!");  
  fprintf(stderr, "xtrsdr v1.0.0 / Test1 start\n");  
  esp_log_level_set("*", ESP_LOG_DEBUG);

  usbhost_begin();
  // make the pushButton pin an input:
  pinMode(buttonPin, INPUT_PULLUP);
  delay(3000);

  uint32_t cpu_freq = getCpuFrequencyMhz();  // Получаем частоту CPU в Гц
  printf("CPU clock: %u MHz\n", cpu_freq);

  printMemoryInfo();

	int i, device_count, device, offset;
	char *s2;
	char vendor[256], product[256], serial[256];  
  device_count = rtlsdr_get_device_count();    
	if (!device_count) {
		fprintf(stderr, "No supported devices found.\n");
    while(1) delay(100);
	}

  fprintf(stderr, "Found %d device(s):\n", device_count);
	for (i = 0; i < device_count; i++) {
		rtlsdr_get_device_usb_strings(i, vendor, product, serial);
		fprintf(stderr, "  %d:  %s, %s, SN: %s\n", i, vendor, product, serial);
	}


  int r = rtlsdr_open(&dev, 0);
	if (r < 0) {
		fprintf(stderr, "Failed to open rtlsdr device #%d.\n", 0);
    while(1) delay(100);
	}
  digitalWrite(LED,HIGH);
  
  //for(int i=0;i<10;i++){ Serial0.println(i);
  //   delay(200);
  //}

  if (direct_sampling)
      verbose_direct_sampling(dev, 2);

/* Set the sample rate */
  verbose_set_sample_rate(dev, samp_rate);

  verbose_set_frequency(dev,frequency);

  if (0 == gain) {
    /* Enable automatic gain */
    verbose_auto_gain(dev);
 } else {
   /* Enable manual gain */
   gain = nearest_gain(dev, gain);
   verbose_gain_set(dev, gain);
 }

  
  verbose_ppm_set(dev, PPM_ERROR);
  
  	/* Reset endpoint before we start reading from it (mandatory) */
	verbose_reset_buffer(dev);

  //SerialConsole();
  fprintf(stderr, "Reading samples in async mode...\n");
  r = rtlsdr_read_async(dev, rtlsdr_callback, 0,30,6400);


  rtlsdr_close(dev);

  digitalWrite(LED,LOW);

}


void loop() {
  // read the pushbutton:
  int buttonState = digitalRead(buttonPin);
  // if the button state has changed,
  if ((buttonState != previousButtonState)
      // and it's currently pressed:
      && (buttonState == LOW)) {
    // increment the button counter
    counter++;
    // type out a message
  }
  // save the current button state for comparison next time:
  previousButtonState = buttonState;
 
  delay(100);
}
