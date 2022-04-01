//esp32
TaskHandle_t Task1;

/* Пауза по первой (или второй) производной, для формовки нового аккумулятора,
  отсечка 14,4 или таймер, плавная раскачка до 12В, защита от КЗ и обратной полярности,
  дифференциальный вход,проверка зависания АЦП.

  https://github.com/baruch/ADS1115



  0.74 -  miltiplier: калибровочный коэффициент для делителя напряжения
  R1 = 18kOhm
  R2 = 12kOhm
  R3 = 18kOhm
  A0 +Vacc
  A1 -Vacc
  A4 - SDA
  A5 - SCL
*/
#include <stdint.h>
#include <ADS1115.h>
#include <WiFi.h>
#include <freertos/task.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>


const char* ssid     = "ESP32-Access-Point";
const char* password = "123456789";

WiFiServer server(80);

#define multiplier .74
#define U50 12300. // напряжение 50% заряда
#define UpLimit 14400  //отсечка сверху
#define OverVoltage 13200  //условие перехода в качели
#define LowLimit 12800  //нижний предел качелей
#define dU 5 // значение первой производной для паузы
#define ChargeRelayPin 15 //SSR-10DA
#define InternalLedPin 13
#define timeoutTime 1500 // сброс http соединения по таймеру
ADS1115 adc;

// Переменные
uint32_t RelaxPeriod = 1000; // Постоянная времени дифференцирования для паузы
uint32_t Timer = 0;
int16_t U1, InitU; // Напряжение/
String InitVoltage, Voltage, Mode, StartCharge, EndCharge, PauseLength, ChargeLength;
uint8_t OverVoltageCounter = 0;// счетчик условий перехода в качели
//Функции
int16_t ReadU() // Вольтметр
{
  int16_t result = adc.read_sample();
  //Serial.print ("result: "); Serial.println (result);
  delay (5); // Пауза для завершения преобразования
  result *= multiplier;
  Voltage = String(result);
  return result;
}
//Процедуры

void ADCinit()
{
  while (adc.trigger_sample() != 0)  //запись управляющего регистра и проверка зависания АЦП
  {
    Serial.println("adc read trigger failed (ads1115 not connected?)");
    Mode = "ADC error";
    delay(1000);
    for (uint8_t i = 1; i <= 4; i++)  //индикация ошибки АЦП, 4 вспышки каждую 1 сек
    {
      digitalWrite(InternalLedPin, HIGH);
      delay(250);
      digitalWrite(InternalLedPin, LOW);
      delay(250);
    }
  }
  delay(250);
}
void relax(uint32_t Period)// Пауза по первой производной
{
  int16_t U2;
  do
  {
    U1 = ReadU();
    vTaskDelay (Period);
    U2 = ReadU();
    // Serial.println (U1-U2);Serial.println ();
  }
  while ((U1 - U2) > dU);
  //Serial.println (U1);Serial.println (U2);

}
/*void relax(uint32_t Period)// Пауза по второй производной
  {
  int16_t U2;
  int16_t dU1, dU2;
  U1 = ReadU();
  do
  {
    delay (Period);
    U2 = ReadU();
    dU1 = U1 - U2;
    delay (Period);
    U1 = ReadU();
    dU2 = U2 - U1;
    Serial.println (dU1); Serial.println (dU2); Serial.println (dU1 - dU2); Serial.println ();
  }
  while ((dU1 - dU2) >= 0);

  }*/
void relayON()
{
  digitalWrite(ChargeRelayPin, HIGH);
  digitalWrite(InternalLedPin, LOW);
}
void relayOFF()
{
  digitalWrite(ChargeRelayPin, LOW);
  digitalWrite(InternalLedPin, HIGH);
}

uint32_t GetTime(uint32_t Timer1)// вычисляем интервал времени с защитой переполнения millis
{
  uint32_t Timer2 = millis();
  if (Timer1 > Timer2)  // Проверка переполнения millis
  {
    Timer2 = 4294967295 - Timer1 + Timer2;
  }
  else
  {
    Timer2 = Timer2 - Timer1;
    return Timer2;
  }
}

/******************************************************************************************/

void setup() {
  pinMode(ChargeRelayPin, OUTPUT); //SSR-10DA
  pinMode(InternalLedPin, OUTPUT); //LED


  adc.begin();
  adc.set_data_rate(ADS1115_DATA_RATE_8_SPS);
  adc.set_mode(ADS1115_MODE_CONTINUOUS);
  adc.set_mux(ADS1115_MUX_DIFF_AIN0_AIN1);
  adc.set_pga(ADS1115_PGA_TWO_THIRDS);

  Serial.begin(115200);





  // Создаем задачу с кодом из функции Task1code(),
  // с приоритетом 1 и выполняемую на ядре 0:
  xTaskCreatePinnedToCore(
    Task1code,   /* Функция задачи */
    "Task1",     /* Название задачи */
    32000,       /* Размер стека задачи */
    NULL,        /* Параметр задачи */
    10,           /* Приоритет задачи */
    NULL,      /* Идентификатор задачи,
                                    чтобы ее можно было отслеживать */
    0);          /* Ядро для выполнения задачи (0) */


  ADCinit();
  InitU = ReadU();
  InitVoltage = String(InitU);
  delay(50);

}

// Функция Task1code: WEB server:
void Task1code( void * pvParameters )
{
  uint32_t currentTime;
  Serial.print("Task1 running on core ");
  //  "Задача Task1 выполняется на ядре "
  Serial.println(xPortGetCoreID());
  ArduinoOTA
  .onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  })
  .onEnd([]() {
    Serial.println("\nEnd");
  })
  .onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  })
  .onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  for (;;)
  {
    String header, WorkTime;
    if  (WiFi.status() != WL_CONNECTED)
    {
      // подключаемся к WiFi-сети при помощи заданных выше SSID и пароля:
      Serial.print("Connecting to ");
      //  "Подключаемся к "
      Serial.println(ssid);
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid, password);
      while (WiFi.status() != WL_CONNECTED)
      {
        vTaskDelay(500);
        Serial.print(".");
      }


      // печатаем в мониторе порта локальный IP-адрес
      // и запускаем веб-сервер:
      Serial.println("");
      Serial.println("WiFi connected.");  //  "WiFi подключен."
      Serial.println("IP address: ");  //  "IP-адрес: "
      Serial.println(WiFi.localIP());
      Serial.print("Power level: ");
      Serial.println(WiFi.RSSI());
      ArduinoOTA.begin();

      server.begin();
      vTaskDelay(100);

    }




    // начинаем прослушивать входящих клиентов:
    WiFiClient client = server.available();
    ArduinoOTA.handle();
    if (client)
    { // если подключился новый клиент,
      currentTime = millis();
      Serial.println("New Client.");  // печатаем сообщение
      // «Новый клиент.»
      // в мониторе порта;

      String currentLine = "";
      while (client.connected() && (GetTime(currentTime) <= timeoutTime))
      { // цикл while() будет работать
        // все то время, пока клиент
        // будет подключен к серверу, но не более чем timeoutTime
        if (client.available())
        { // если у клиента есть данные,
          // которые можно прочесть,
          char c = client.read();     // считываем байт, а затем
          Serial.write(c);            // печатаем его в мониторе порта
          header += c;
          if (c == '\n')
          { // если этим байтом является
            // символ новой строки
            // если мы получим два символа новой строки подряд
            // то это значит, что текущая строчка пуста;
            // это конец HTTP-запроса клиента,
            // а значит – пора отправлять ответ:
            if (currentLine.length() == 0)
            {
              // HTTP-заголовки всегда начинаются
              // с кода ответа (например, «HTTP/1.1 200 OK»)
              // и информации о типе контента
              // (чтобы клиент понимал, что получает);
              // в конце пишем пустую строчку:
              client.println("HTTP/1.1 200 OK");
              client.println("Content-type:text/html");
              client.println("Connection: close");
              //  "Соединение: отключено"
              client.println();
              WorkTime = String(currentTime / 3600000) + "hours, " + (currentTime % 3600000 / 60000) + "min., " + (currentTime % 60000 / 1000) + "sec.";
              // показываем веб-страницу с помощью этого HTML-кода:
              client.println("<!DOCTYPE html><html>");
              client.println("<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
              client.println("<link rel=\"icon\" href=\"data:,\">");
              client.println("<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}");
              client.println("</style></head>");

              // заголовок веб-страницы:
              client.println("<body><h1>ESP32 Web Server</h1>");
              client.println("<h2>Lead-acid battery charger</h2>");
              client.println("<p>Mode " + Mode + "</p>");
              client.println("<p>Current voltage <b>" + Voltage + "</b>mV</p>");
              client.println("<p>Start voltage <b>" + StartCharge + "</b>mV</p>");
              client.println("<p>Stop voltage <b>" + EndCharge + "</b>mV</p>");
              client.println("<p>Charge impulse length <b>" + ChargeLength + "</b>ms</p>");
              client.println("<p>Pause length <b>" + PauseLength + "</b>ms</p>");
              client.println("<p>Initial voltage <b>" + InitVoltage + "</b>mV</p>");
              client.println("<p>Working <b>" + WorkTime + "</b></p>");
              client.println("</body></html>");
              // конец HTTP-ответа задается
              // с помощью дополнительной пустой строки:
              client.println();
              // очищаем переменную «header»:
              header = "";
              // отключаем соединение:
              client.stop();
              Serial.println("Client disconnected.");
              //  "Клиент отключен."
              Serial.println("");
              // выходим из цикла while:
              break;
            }
          }
        }
      }
    }
    client.stop();
    vTaskDelay(100);
  }
}



/***********************************************************************************/

void loop()
{
  uint32_t Timer, ChargePeriod;
  U1 = ReadU();

  while (U1 == 0) //защита от КЗ или зависания АЦП
  {
    ADCinit();
    U1 = ReadU();
    Mode = "Short circuit or ADC error";
  }
  while (U1 < 0) //защита от обратной полярности, непрерывно часто мигаем светодиодом
  {
    Mode = "Negative polarity!";
    Serial.println ("U1 < 0: "); Serial.println (U1);
    digitalWrite(InternalLedPin, HIGH);
    delay(250);
    digitalWrite(InternalLedPin, LOW);
    delay(250);
    U1 = ReadU();
  }


  while ((U1 > 0) && (U1 < 12000)) //раскачка до 12В
  {
    Mode = "Charge until 12V";
    Serial.print ("(U1 >= 0) && (U1 < 12000): ");  Serial.println (U1);
    relayON();
    delay (int((U1 / 12000.) * 2048)); // Подача 0-2 сек
    relayOFF();
    delay (1024); // Пауза 1 сек
    U1 = ReadU();
  }

  Timer = millis();//Засекаем начало паузы
  Mode = "Pause";
  relax(RelaxPeriod); // Пауза в цикле
  Timer = GetTime(Timer);//Измеряем длительность паузы
  PauseLength = String(Timer);
  U1 = ReadU();
  StartCharge = String(U1);
  Serial.print ("Start Charge: "); Serial.println (U1);
  //качели
  if (U1 >= OverVoltage) //отсеиваем случайные значения из-за нестабильности питания для перехода в качели
  {
    OverVoltageCounter++;
  }
  else
  {
    OverVoltageCounter = 0;
  }
  if (OverVoltageCounter >= 5)   //переход в качели после 5 срабатываний счетчика
  {
    Mode = "Storage";
    digitalWrite(InternalLedPin, LOW);
    do
    {
      U1 = ReadU();
      delay(5000);
      for (uint8_t i = 1; i <= 2; i++)  //индикация качелей 2 вспышки каждые 5 сек
      {
        digitalWrite(InternalLedPin, HIGH);
        delay(500);
        digitalWrite(InternalLedPin, LOW);
        delay(500);
      }
    }
    while (U1 >= LowLimit);
    OverVoltageCounter = 0;
  }
  //конец качелей

  if (U1 >= 12000) // Основной заряд
  {
    Mode = "Main charge";
    RelaxPeriod = long(pow(2.72, ((U1 - U50) / 1024)) * 1024); // вычисление постоянной дифференцирования для текущего цикла по напряжению в конце паузы
    ChargePeriod = long(pow(2.72, ((U50 - U1) / 1024)) * Timer); //защита от вечного заряда, вычисление времени отсечки для текущего цикла по длительности паузы
    //Подача зарядного импульса
    Timer = millis();//засекаем начало зарядного импульса
    relayON();
    while ((U1 <= UpLimit) && (GetTime(Timer) <= ChargePeriod))//отсечка по напряжению или времени
    {
      U1 = ReadU();
    }
    relayOFF();
    Timer = GetTime(Timer);//Измеряем длительность зарядного импульса
    ChargeLength = String(Timer);

    if (U1 < UpLimit)
    {
      OverVoltageCounter = 0; //Сбрасываем счетчик перехода в качели, если отсечка только по времени
    }
    EndCharge = String(U1);
    Serial.print ("END Charge: ");  Serial.println (U1);
  }
  //Конец подачи зарядного импульса

}
