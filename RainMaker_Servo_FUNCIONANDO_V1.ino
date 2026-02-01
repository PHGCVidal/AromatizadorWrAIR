
#include "qrcode.h"
#include "RMaker.h"
#include "WiFi.h"
#include "WiFiProv.h"
#include "AppInsights.h"
#include <ESP32Servo.h>
#include "time.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>


#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

#define DEFAULT_POWER_MODE false // Começa desligado
const char *service_name = "PROV_aromatizador";
const char *pop = "abcd1234";

// --- CONFIGURAÇÃO MECÂNICA ---
#define PINO_SERVO 18
#define ANGULO_REPOUSO 0   // Posição de descanso
int angulo_aperto = 100; // Posição que aperta o spray (ajustaremos depois)
#define TEMPO_APERTO   1000 // Tempo segurando o spray apertado (ms)

Servo meuServo;
// --- CONTROLE DE DISPAROS E COOLDOWN ---
int qtd_disparos = 1;          // Quantidade padrão
bool sistema_ocupado = false;  // Trava para não aceitar cliques simultâneos
unsigned long momento_liberacao = 0; // Marca quando o sistema pode ser usado de novo
#define TEMPO_COOLDOWN 5000    // 5 Segundos de descanso obrigatório após o ciclo
#define TEMPO_ENTRE_LOOPS 1500 // Tempo entre sprays (se for mais de 1)
// --- VARIÁVEIS DO MODO NÃO PERTURBE (DND) ---
bool dnd_ativo = false;    
float dnd_inicio = 22.0;   // Agora é float (22.5 seria 22:30)
float dnd_fim = 7.0;       // Agora é float

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

String getHoraAtual() {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        return "Erro Relogio";
    }
    char timeStringBuff[50];
    strftime(timeStringBuff, sizeof(timeStringBuff), "%d/%m as %H:%M", &timeinfo);
    return String(timeStringBuff);
}

// --- FUNÇÃO AUXILIAR PARA DESENHAR NO OLED ---
void exibirQRCodeNoDisplay(const char* nome, const char* pop, const char* transporte) {
    // 1. Monta o JSON exato que o RainMaker exige
    String qr_json = "{\"ver\":\"v1\",\"name\":\"";
    qr_json += nome;
    qr_json += "\",\"pop\":\"";
    qr_json += pop;
    qr_json += "\",\"transport\":\"";
    qr_json += transporte;
    qr_json += "\"}";

    // 2. Gera os dados do QR Code
    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(3)]; // Versão 3 (29x29) é ideal para 128x64
    qrcode_initText(&qrcode, qrcodeData, 3, ECC_LOW, qr_json.c_str());

    // 3. Limpa e Centraliza
    display.clearDisplay();
    int escala = 2; // Cada ponto vale 2 pixels (para ficar visível)
    int x_offset = (128 - (qrcode.size * escala)) / 2;
    int y_offset = (64 - (qrcode.size * escala)) / 2;

    // 4. Desenha Pixel a Pixel
    display.fillScreen(SSD1306_BLACK); // Fundo preto
    for (uint8_t y = 0; y < qrcode.size; y++) {
        for (uint8_t x = 0; x < qrcode.size; x++) {
            if (qrcode_getModule(&qrcode, x, y)) {
                display.fillRect(x_offset + (x * escala), y_offset + (y * escala), escala, escala, SSD1306_WHITE);
            }
        }
    }
    
    // Pequeno texto de ajuda no topo
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    // Se quiser escrever algo, descomente abaixo:
    // display.print("Scan App:"); 
    
    display.display();
}

void sysProvEvent(arduino_event_t *sys_event)
{
    switch (sys_event->event_id) {
    case ARDUINO_EVENT_PROV_START:
#if CONFIG_IDF_TARGET_ESP32S2
        Serial.printf("\nProvisioning Started with name \"%s\" and PoP \"%s\" on SoftAP\n", service_name, pop);
        printQR(service_name, pop, "softap");
        
        // --- ADICIONADO PARA O DISPLAY (SOFTAP) ---
        exibirQRCodeNoDisplay(service_name, pop, "softap");
        // ------------------------------------------
#else
        Serial.printf("\nProvisioning Started with name \"%s\" and PoP \"%s\" on BLE\n", service_name, pop);
        printQR(service_name, pop, "ble");

        // --- ADICIONADO PARA O DISPLAY (BLE) ---
        exibirQRCodeNoDisplay(service_name, pop, "ble");
        // ---------------------------------------
#endif
        break;
        
    case ARDUINO_EVENT_PROV_INIT:
        wifi_prov_mgr_disable_auto_stop(10000);
        break;
        
    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
        wifi_prov_mgr_stop_provisioning();
        // Opcional: Avisar na tela que deu certo
        display.clearDisplay();
        display.setCursor(0,25); display.setTextSize(2);
        display.println("CONECTADO!");
        display.display();
        break;
        
    default:;
    }
}

bool verificarPodeDisparar() {
    if (!dnd_ativo) return true; // Se DND desligado, libera geral

    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        Serial.println("Erro Relogio. Liberando disparo.");
        return true; 
    }

    // 1. Converte TUDO para minutos (0 a 1440)
    int minutos_agora = (timeinfo.tm_hour * 60) + timeinfo.tm_min;
    int minutos_inicio = dnd_inicio * 60; // Ex: 22.5 * 60 = 1350
    int minutos_fim = dnd_fim * 60;       // Ex: 7.0 * 60 = 420

    Serial.printf("DND Check (min): Agora: %d | Inicio: %d | Fim: %d\n", minutos_agora, minutos_inicio, minutos_fim);

    bool dentro_do_horario = false;

    // 2. Lógica Matemática
    if (minutos_inicio < minutos_fim) {
        // Bloqueio no mesmo dia (Ex: 13:00 as 15:30)
        if (minutos_agora >= minutos_inicio && minutos_agora < minutos_fim) {
            dentro_do_horario = true;
        }
    } else {
        // Bloqueio vira a noite (Ex: 22:30 as 07:00)
        // Bloqueia se for MAIOR que o inicio OU MENOR que o fim
        if (minutos_agora >= minutos_inicio || minutos_agora < minutos_fim) {
            dentro_do_horario = true;
        }
    }

    if (dentro_do_horario) {
        Serial.println(">>> BLOQUEADO: MODO NAO PERTURBE <<<");
        return false; 
    }
    return true; 
}

// --- FUNÇÃO QUE EXECUTA O TRABALHO SUJO ---
void executarDisparo(String origem) {

  // Verifica se está no horário de silêncio
    if (!verificarPodeDisparar()) {
        // Se for bloqueado, desliga o botão no app e sai
        if (my_switch) my_switch->updateAndReportParam("Power", false);
        return; 
    }
    // --------------------------------
    
    // 1. O GUARDIÃO (TRAVA ANTI-SPAM)
    // Se já estiver trabalhando OU se ainda estiver no tempo de descanso...
    if (sistema_ocupado || millis() < momento_liberacao) {
        Serial.println("COMANDO BLOQUEADO: Sistema ocupado ou esfriando.");
        
        // Se foi chamado pelo App, forçamos o botão a desligar visualmente
        if (my_switch) my_switch->updateAndReportParam("Power", false);
        return; // Sai da função sem fazer nada!
    }
    
    sistema_ocupado = true; // Ativa a trava (Estou ocupado!)

    // 2. O CICLO DE TRABALHO (Loop de repetição)
    for (int i = 1; i <= qtd_disparos; i++) {
        Serial.printf(">>> Executando Spray %d de %d\n", i, qtd_disparos);
        
        // Atualiza Tela
        String status = "SPRAY " + String(i) + "/" + String(qtd_disparos);
        drawScreen(status, origem);
        
        digitalWrite(gpio_switch, HIGH); // Liga LED

        // Movimento do Servo
        meuServo.attach(PINO_SERVO);
        meuServo.write(angulo_aperto);
        delay(TEMPO_APERTO); 

        meuServo.write(ANGULO_REPOUSO);
        
        // Se tiver mais disparos pela frente, espera o tempo de recarga
        if (i < qtd_disparos) {
            delay(TEMPO_ENTRE_LOOPS); 
        } else {
            // Se for o último, espera só um pouco e desliga o motor
            delay(500);
            meuServo.detach();
        }
    }

    // 3. FINALIZAÇÃO E REGISTRO
    String hora = getHoraAtual();
    if (my_switch) {
        my_switch->updateAndReportParam("Ultimo Disparo", hora.c_str());
    }

    digitalWrite(gpio_switch, LOW);
    
    // 4. ATIVA O MODO RESFRIAMENTO
    // Só aceita novos comandos daqui a 5 segundos
    momento_liberacao = millis() + TEMPO_COOLDOWN; 
    
    sistema_ocupado = false; // Libera a trava lógica de ocupado
    
    // Avisa no App que terminou (Desliga o botão)
    if (my_switch) my_switch->updateAndReportParam("Power", false);

    // Feedback visual de descanso
    drawScreen("AGUARDE...", "Resfriando");
    delay(2000); 
    drawScreen("ONLINE", hora);
}

void write_callback(Device *device, Param *param, const param_val_t val,
                    void *priv_data, write_ctx_t *ctx)
{
    const char *param_name = param->getParamName();
    Serial.printf("RECEBIDO: Parametro = %s | Valor = %s\n", param_name, val.val.b ? "TRUE" : "FALSE");

    // SE CLICAREM NO BOTÃO DE LIGAR
    if (strcmp(param_name, "Power") == 0) { 
        if (val.val.b == true) {
            // Tenta executar (a função lá em cima decide se obedece ou ignora)
            executarDisparo("Via App"); 
        } else {
            // Se mandarem desligar, apenas garantimos LED apagado
            digitalWrite(gpio_switch, LOW);
            param->updateAndReport(val);
        }
    }
    
    // SE MEXEREM NO SLIDER DE QUANTIDADE
    if (strcmp(param_name, "Qtd Sprays") == 0) {
        qtd_disparos = val.val.i;
        Serial.printf("Nova Qtd Configurada: %d\n", qtd_disparos);
        param->updateAndReport(val);
    }
    if (strcmp(param_name, "Angulo") == 0) {
        angulo_aperto = val.val.i;
        Serial.printf("Novo Angulo Calibrado: %d\n", angulo_aperto);
        param->updateAndReport(val);
    }
    if (strcmp(param_name, "Modo Noturno") == 0) {
        dnd_ativo = val.val.b;
        param->updateAndReport(val);
    }

    if (strcmp(param_name, "DND Inicio") == 0) {
        dnd_inicio = val.val.f; 
        Serial.printf("Novo Inicio DND: %.1f\n", dnd_inicio);
        param->updateAndReport(val);
    }

    if (strcmp(param_name, "DND Fim") == 0) {
        dnd_fim = val.val.f;    
        Serial.printf("Novo Fim DND: %.1f\n", dnd_fim);
        param->updateAndReport(val);
    }
}

void drawScreen(String textoPrincipal, String textoSecundario) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE); // Garante a cor certa na biblioteca Adafruit
    
    // --- ÁREA RESERVADA PARA IMAGEM (FUTURO) ---
    // display.drawBitmap(0, 0, seuBitmap, 128, 64, SSD1306_WHITE);
    // -------------------------------------------

    // Cabeçalho Fixo (Agora neutro)
    display.setTextSize(1);
    display.setCursor(10, 0); // Empurrei um pouco pra direita pra centralizar
    display.println("SISTEMA AROMA"); 
    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

    // Texto Principal (Grande)
    display.setTextSize(2);
    display.setCursor(0, 25);
    display.println(textoPrincipal);

    // Texto Secundário (Rodapé)
    display.setTextSize(1);
    display.setCursor(0, 55);
    display.println(textoSecundario);

    display.display();
}

void setup()
{
    Serial.begin(115200);
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
        Serial.println("Erro no Display OLED");
    }
    drawScreen("INICIANDO", "Sistema v3.2");
    pinMode(gpio_0, INPUT);
    pinMode(gpio_switch, OUTPUT);
    digitalWrite(gpio_switch, DEFAULT_POWER_MODE);

    // Configuração Inicial do Servo
    ESP32PWM::allocateTimer(0);
    meuServo.setPeriodHertz(60);
    meuServo.attach(PINO_SERVO);
    meuServo.write(ANGULO_REPOUSO); // Garante que inicia em 0
    delay(500);
    meuServo.detach();

    Node my_node;
    my_node = RMaker.initNode("Aromatizador-ESP32");

    my_switch = new Switch("Switch", &gpio_switch);
    if (!my_switch) {
        return;
    }
    my_switch->addParam(Param("Ultimo Disparo", "esp.param.text", value("Aguardando..."), PROP_FLAG_READ));
    // CONFIGURA O SLIDER DE QUANTIDADE (1 a 5)
    Param qtd("Qtd Sprays", ESP_RMAKER_PARAM_RANGE, value(qtd_disparos), PROP_FLAG_READ | PROP_FLAG_WRITE);
    qtd.addBounds(value(1), value(5), value(1)); // Mínimo 1, Máximo 5
    qtd.addUIType(ESP_RMAKER_UI_SLIDER);
    my_switch->addParam(qtd);

    // Slider de CALIBRAGEM (Essencial para seus testes mecânicos)
    Param calibra("Angulo", ESP_RMAKER_PARAM_RANGE, value(angulo_aperto), PROP_FLAG_READ | PROP_FLAG_WRITE);
    calibra.addBounds(value(0), value(180), value(1));
    calibra.addUIType(ESP_RMAKER_UI_SLIDER);
    my_switch->addParam(calibra);

    // --- MODO NÃO PERTURBE (DND) ---
    
    // 1. Switch de Ligar/Desligar
    Param dnd_sw("Modo Noturno", ESP_RMAKER_PARAM_POWER, value(dnd_ativo), PROP_FLAG_READ | PROP_FLAG_WRITE);
    dnd_sw.addUIType(ESP_RMAKER_UI_TOGGLE);
    my_switch->addParam(dnd_sw);

    // 2. Slider Hora Inicio (0.0 a 24.0 com passo de 0.5)
    Param dnd_ini("DND Inicio", ESP_RMAKER_PARAM_RANGE, value(dnd_inicio), PROP_FLAG_READ | PROP_FLAG_WRITE);
    dnd_ini.addBounds(value(0.0f), value(24.0f), value(0.5f)); // Passo de 30 min (0.5)
    dnd_ini.addUIType(ESP_RMAKER_UI_SLIDER);
    my_switch->addParam(dnd_ini);

    // 3. Slider Hora Fim (0.0 a 24.0 com passo de 0.5)
    Param dnd_end("DND Fim", ESP_RMAKER_PARAM_RANGE, value(dnd_fim), PROP_FLAG_READ | PROP_FLAG_WRITE);
    dnd_end.addBounds(value(0.0f), value(24.0f), value(0.5f)); // Passo de 30 min (0.5)
    dnd_end.addUIType(ESP_RMAKER_UI_SLIDER);
    my_switch->addParam(dnd_end);
    
    my_switch->addCb(write_callback);
    my_node.addDevice(*my_switch);

    // Mantendo todas as funções vitais do RainMaker
    RMaker.enableOTA(OTA_USING_TOPICS);
    RMaker.enableSchedule();
    RMaker.enableScenes();
    initAppInsights();
    RMaker.enableSystemService(SYSTEM_SERV_FLAGS_ALL, 2, 2, 2);
    RMaker.setTimeZone("America/Sao_Paulo");
    RMaker.enableTZService();

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
          // Clique Rápido (Agora usa a mesma lógica do App com a quantidade configurada)
          Serial.println("Botão Físico: Iniciando sequência...");
          
          // Avisa o App que ligou
          if (my_switch) my_switch->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, true);
          
          executarDisparo("Botao Fisico");
          
          // Avisa o App que desligou
          if (my_switch) my_switch->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, false);
        }
    }
    static unsigned long ultimoUpdate = 0;
    if (millis() - ultimoUpdate > 60000) { // A cada 1 minuto
        drawScreen("ONLINE", getHoraAtual());
        ultimoUpdate = millis();
    }
    delay(100);
}
