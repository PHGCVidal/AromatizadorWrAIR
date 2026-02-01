#include "RMaker.h"
#include "WiFi.h"
#include "WiFiProv.h"
#include "AppInsights.h"
#include <ESP32Servo.h>

#define DEFAULT_POWER_MODE false // Começa desligado
const char *service_name = "PROV_aromatizador";
const char *pop = "abcd1234";

// --- CONFIGURAÇÃO MECÂNICA ---
#define PINO_SERVO 18
#define ANGULO_REPOUSO 0   // Posição de descanso
#define ANGULO_APERTO  170 // Posição que aperta o spray (ajustaremos depois)
#define TEMPO_APERTO   1000 // Tempo segurando o spray apertado (ms)

Servo meuServo;

// GPIO for push button (BOOT)
#if CONFIG_IDF_TARGET_ESP32C3
static int gpio_0 = 9;
static int gpio_switch = 7;
#else
static int gpio_0 = 0;
static int gpio_switch = 2;
#endif

// Estado interno
bool switch_state = false;
static Switch *my_switch = NULL;

void sysProvEvent(arduino_event_t *sys_event)
{
    switch (sys_event->event_id) {
    case ARDUINO_EVENT_PROV_START:
#if CONFIG_IDF_TARGET_ESP32S2
        Serial.printf("\nProvisioning Started with name \"%s\" and PoP \"%s\" on SoftAP\n", service_name, pop);
        printQR(service_name, pop, "softap");
#else
        Serial.printf("\nProvisioning Started with name \"%s\" and PoP \"%s\" on BLE\n", service_name, pop);
        printQR(service_name, pop, "ble");
#endif
        break;
    case ARDUINO_EVENT_PROV_INIT:
        wifi_prov_mgr_disable_auto_stop(10000);
        break;
    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
        wifi_prov_mgr_stop_provisioning();
        break;
    default:;
    }
}

// --- AQUI ESTÁ A MUDANÇA PARA O "GATILHO" ---
void write_callback(Device *device, Param *param, const param_val_t val,
                    void *priv_data, write_ctx_t *ctx)
{
    const char *device_name = device->getDeviceName();
    const char *param_name = param->getParamName();

    if (strcmp(param_name, "Power") == 0) {
        Serial.printf("Comando recebido: %s\n", val.val.b ? "ATIVAR" : "PARAR");

        // Só executamos a ação se o comando for "LIGAR" (true)
        if (val.val.b == true) {

            // 1. Liga o LED para indicar funcionamento
            digitalWrite(gpio_switch, HIGH);

            // 2. Movimento de "Ataque" (Aperta o Spray)
            Serial.println(">>> Apertando Spray...");
            meuServo.attach(PINO_SERVO);
            meuServo.write(ANGULO_APERTO);

            // Espera o tempo necessário para o servo chegar lá e apertar bem
            delay(TEMPO_APERTO);

            // 3. Movimento de "Retorno" (Solta o Spray)
            Serial.println("<<< Soltando Spray...");
            meuServo.write(ANGULO_REPOUSO);
            delay(600); // Tempo para voltar suavemente
            meuServo.detach(); // Desliga o motor

            // 4. Desliga o LED
            digitalWrite(gpio_switch, LOW);

            // 5. O TRUQUE: Manda o botão do App voltar para "Desligado" sozinho
            param->updateAndReport(value(false));

        } else {
            // Se alguém mandar "false", só garante que tá tudo desligado
            digitalWrite(gpio_switch, LOW);
            param->updateAndReport(val);
        }
    }
}

void setup()
{
    Serial.begin(115200);
    pinMode(gpio_0, INPUT);
    pinMode(gpio_switch, OUTPUT);
    digitalWrite(gpio_switch, DEFAULT_POWER_MODE);

    // Configuração Inicial do Servo
    ESP32PWM::allocateTimer(0);
    meuServo.setPeriodHertz(50);
    meuServo.attach(PINO_SERVO);
    meuServo.write(ANGULO_REPOUSO); // Garante que inicia em 0
    delay(500);
    meuServo.detach();

    Node my_node;
    my_node = RMaker.initNode("ESP RainMaker Node");

    my_switch = new Switch("Switch", &gpio_switch);
    if (!my_switch) {
        return;
    }

    my_switch->addCb(write_callback);
    my_node.addDevice(*my_switch);

    // Mantendo todas as funções vitais do RainMaker
    RMaker.enableOTA(OTA_USING_TOPICS);
    RMaker.enableTZService();
    RMaker.enableSchedule();
    RMaker.enableScenes();
    initAppInsights();
    RMaker.enableSystemService(SYSTEM_SERV_FLAGS_ALL, 2, 2, 2);

    RMaker.start();

    WiFi.onEvent(sysProvEvent);
#if CONFIG_IDF_TARGET_ESP32S2
    WiFiProv.beginProvision(WIFI_PROV_SCHEME_SOFTAP, WIFI_PROV_SCHEME_HANDLER_NONE, WIFI_PROV_SECURITY_1, pop, service_name);
#else
    WiFiProv.beginProvision(WIFI_PROV_SCHEME_BLE, WIFI_PROV_SCHEME_HANDLER_FREE_BTDM, WIFI_PROV_SECURITY_1, pop, service_name);
#endif
}

void loop()
{
    // Lógica do Botão Físico (BOOT) também virou Gatilho Automático
    if (digitalRead(gpio_0) == LOW) {
        delay(100);
        int startTime = millis();
        while (digitalRead(gpio_0) == LOW) delay(50);
        int endTime = millis();

        if ((endTime - startTime) > 10000) {
            Serial.printf("Reset to factory.\n");
            RMakerFactoryReset(2);
        } else if ((endTime - startTime) > 3000) {
            Serial.printf("Reset Wi-Fi.\n");
            RMakerWiFiReset(2);
        } else {
            // Clique Rápido no botão físico -> Dispara o ciclo completo
            Serial.println("Botão Físico: Ciclo de Disparo!");

            digitalWrite(gpio_switch, HIGH);

            // Atualiza o App dizendo que ativou
            if (my_switch) my_switch->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, true);

            meuServo.attach(PINO_SERVO);
            meuServo.write(ANGULO_APERTO);
            delay(TEMPO_APERTO);

            meuServo.write(ANGULO_REPOUSO);
            delay(600);
            meuServo.detach();

            digitalWrite(gpio_switch, LOW);

            // Atualiza o App dizendo que terminou
            if (my_switch) my_switch->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, false);
        }
    }
    delay(100);
}
