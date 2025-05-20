#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include "numeros.h"
#include "pico/bootrom.h"
#include "hardware/timer.h"
#include "hardware/pwm.h"
#include "hardware/i2c.h"
#include "lib/ssd1306.h"
#include "lib/font.h"
#include <math.h>

// ----------------------------- DEFINIÇÕES DE PINOS -----------------------------
#define MATRIZ_LED_PIN 7
#define WIFI_SSID "Tesla"
#define WIFI_PASSWORD "123456788"
#define LED_BLUE_PIN 12
#define LED_GREEN_PIN 11
#define LED_RED_PIN 13
#define BUZZER_PIN 10
#define BUTTON_A_GPIO 5
#define BUTTON_B_GPIO 6
#define JOYSTICK_X_PIN 26 // GP26 (ADC0) para eixo X
#define JOYSTICK_Y_PIN 27 // GP27 (ADC1) para eixo Y
#define I2C_PORT i2c1
#define I2C_SDA 14
#define I2C_SCL 15
#define endereco 0x3C

int setor_1comando = 0;
int setor_2comando = 0;
int setor_3comando = 0;
int setor_4comando = 0;
int setor_atual = 1;

struct repeating_timer meu_timer;
bool alarme_ativo = false;
bool botao_a_press = false;
bool wifi_connected = true;
bool automatic_mode = true;
bool sector1_on = true;
bool sector2_on = false;
bool sector3_on = true;
bool sector4_on = false;

#define ATRASO_CONTADOR 3000
char *modo_atual = "Manual";
char setores_ativos[50];
#define DEBOUNCE_DELAY_MS 200

uint32_t last_interrupt_time_a = 0;
uint32_t last_interrupt_time_b = 0;

bool sai1 = true;
bool sai2 = true;
bool sai3 = true;
bool sai4 = true;

// Variáveis para rastrear o setor ativo anterior (para evitar atualizações redundantes)
int ultimo_setor_joystick = 0;

// Funções
void gpio_led_bitdog(void);
void iniciarModoAutomatico();
void pararModoAutomatico();
void iniciar_pwm_buzzer(uint freq_hz);
void parar_pwm_buzzer();
void atualizarSetoresAtivos();
void toggle_mode();
void update_display(ssd1306_t *ssd);
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
float temp_read(void);
void user_request(char **request);
void read_joystick(uint16_t *x, uint16_t *y);

bool precisaAtualizar = false;

bool repeating_timer_callback(struct repeating_timer *t)
{
    if (strcmp(modo_atual, "Automatico") == 0)
    {
        precisaAtualizar = true;
        setor_atual++;
        if (setor_atual > 4)
            setor_atual = 1;
        printf("Modo automático ativo - setor atual: %d\n", setor_atual);
    }
    return true;
}

void gpio_irq_handler(uint gpio, uint32_t events)
{
    uint32_t current_time = to_ms_since_boot(get_absolute_time());
    if (gpio == BUTTON_A_GPIO)
    {
        if (current_time - last_interrupt_time_a > DEBOUNCE_DELAY_MS)
        {
            last_interrupt_time_a = current_time;
            botao_a_press = !botao_a_press;
            printf("Botão A pressionado: %s\n", botao_a_press ? "Ativado" : "Desativado");
        }
    }
    else if (gpio == BUTTON_B_GPIO)
    {
        if (current_time - last_interrupt_time_b > DEBOUNCE_DELAY_MS)
        {
            last_interrupt_time_b = current_time;
            reset_usb_boot(0, 0);
        }
    }
}

// Função para ler os valores dos eixos X e Y do joystick
void read_joystick(uint16_t *x, uint16_t *y)
{
    adc_select_input(0); // ADC0 (GP26) para eixo X
    *x = adc_read();
    adc_select_input(1); // ADC1 (GP27) para eixo Y
    *y = adc_read();
}

int main()
{
    stdio_init_all();
    npInit(MATRIZ_LED_PIN);

    // Inicializa ADC para joystick
    adc_init();
    adc_gpio_init(JOYSTICK_X_PIN);     // GP26
    adc_gpio_init(JOYSTICK_Y_PIN);     // GP27
    adc_set_temp_sensor_enabled(true); // Mantém sensor de temperatura ativo

    gpio_init(BUTTON_B_GPIO);
    gpio_set_dir(BUTTON_B_GPIO, GPIO_IN);
    gpio_pull_up(BUTTON_B_GPIO);
    gpio_set_irq_enabled_with_callback(BUTTON_B_GPIO, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_handler);

    gpio_init(BUTTON_A_GPIO);
    gpio_set_dir(BUTTON_A_GPIO, GPIO_IN);
    gpio_pull_up(BUTTON_A_GPIO);
    gpio_set_irq_enabled_with_callback(BUTTON_A_GPIO, GPIO_IRQ_EDGE_FALL, true, &gpio_irq_handler);

    DesligaMatriz();
    Atualizar_Setor(1, setor_1comando);
    Atualizar_Setor(2, setor_2comando);
    Atualizar_Setor(3, setor_3comando);
    Atualizar_Setor(4, setor_4comando);

    // leds rgb
    gpio_init(LED_GREEN_PIN);
    gpio_init(LED_RED_PIN);
    gpio_set_dir(LED_GREEN_PIN, GPIO_OUT);
    gpio_set_dir(LED_RED_PIN, GPIO_OUT);
    gpio_put(LED_GREEN_PIN, 0);
    gpio_put(LED_RED_PIN, 0);

    strcpy(setores_ativos, "Nenhum");

    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    ssd1306_t ssd;
    ssd1306_init(&ssd, WIDTH, HEIGHT, false, endereco, I2C_PORT);
    ssd1306_config(&ssd);
    ssd1306_send_data(&ssd);
    ssd1306_fill(&ssd, false);
    ssd1306_send_data(&ssd);

    bool cor = true;
    int setor_selecionado = 0;
    update_display(&ssd);
    gpio_led_bitdog();

    while (cyw43_arch_init())
    {
        printf("Falha ao inicializar Wi-Fi\n");
        sleep_ms(100);
        return -1;
    }

    cyw43_arch_gpio_put(LED_PIN, 0);
    cyw43_arch_enable_sta_mode();

    printf("Conectando ao Wi-Fi...\n");
    while (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 20000))
    {
        printf("Falha ao conectar ao Wi-Fi\n");
        sleep_ms(100);
        return -1;
    }
    printf("Conectado ao Wi-Fi\n");

    if (netif_default)
    {
        printf("IP do dispositivo: %s\n", ipaddr_ntoa(&netif_default->ip_addr));
    }

    struct tcp_pcb *server = tcp_new();
    if (!server)
    {
        printf("Falha ao criar servidor TCP\n");
        return -1;
    }

    if (tcp_bind(server, IP_ADDR_ANY, 80) != ERR_OK)
    {
        printf("Falha ao associar servidor TCP à porta 80\n");
        return -1;
    }

    server = tcp_listen(server);
    tcp_accept(server, tcp_server_accept);
    printf("Servidor ouvindo na porta 80\n");

    while (true)
    {
        update_display(&ssd);

        // Controle do joystick no modo manual
        if (strcmp(modo_atual, "Manual") == 0)
        {
            uint16_t x, y;
            read_joystick(&x, &y);
            gpio_put(LED_GREEN_PIN, 0);
            gpio_put(LED_RED_PIN, 1);

            // Mapeamento dos setores com base nos valores do joystick
            if (sai4 & (x >= 0 && x <= 1000 && y >= 3000 && y <= 4095))
            {
                setor_4comando = !setor_4comando;
                Atualizar_Setor(4, setor_4comando);
                iniciar_pwm_buzzer(100);
                sleep_ms(100);
                parar_pwm_buzzer();
                atualizarSetoresAtivos();
                sai4 = false;
            }
            else if (sai2 & (x >= 3000 && x <= 4095 && y >= 3000 && y <= 4095))
            {
                setor_2comando = !setor_2comando;
                Atualizar_Setor(2, setor_2comando);
                iniciar_pwm_buzzer(100);
                sleep_ms(100);
                parar_pwm_buzzer();
                atualizarSetoresAtivos();
                sai2 = false;
            }
            else if (sai3 & (x >= 0 && x <= 1000 && y >= 0 && y <= 1000))
            {
                setor_3comando = !setor_3comando;
                Atualizar_Setor(3, setor_3comando);
                iniciar_pwm_buzzer(100);
                sleep_ms(100);
                parar_pwm_buzzer();
                atualizarSetoresAtivos();
                sai3 = false;
            }
            else if (sai1 & (x >= 3000 && x <= 4095 && y >= 0 && y <= 1000))
            {
                setor_1comando = !setor_1comando;
                Atualizar_Setor(1, setor_1comando);
                iniciar_pwm_buzzer(100);
                sleep_ms(100);
                parar_pwm_buzzer();
                atualizarSetoresAtivos();
                sai1 = false;
            }

            if (!(x >= 3000 && x <= 4095 && y >= 0 && y <= 1000))
            {
                sai1 = true;
            }
            if (!(x >= 3000 && x <= 4095 && y >= 3000 && y <= 4095))
            {
                sai2 = true;
            }
            if (!(x >= 0 && x <= 1000 && y >= 0 && y <= 1000))
            {
                sai3 = true;
            }
            if (!(x >= 0 && x <= 1000 && y >= 3000 && y <= 4095))
            {
                sai4 = true;
            }
        }
        else
        {
            gpio_put(LED_GREEN_PIN, 1);
            gpio_put(LED_RED_PIN, 0);
        }

        if (precisaAtualizar)
        {
            Atualizar_Setor(1, 0);
            Atualizar_Setor(2, 0);
            Atualizar_Setor(3, 0);
            Atualizar_Setor(4, 0);
            setor_1comando = 0;
            setor_2comando = 0;
            setor_3comando = 0;
            setor_4comando = 0;
            switch (setor_atual)
            {
            case 1:
                setor_1comando = 1;
                break;
            case 2:
                setor_2comando = 1;
                break;
            case 3:
                setor_3comando = 1;
                break;
            case 4:
                setor_4comando = 1;
                break;
            default:
                break;
            }
            Atualizar_Setor(setor_atual, 1);
            precisaAtualizar = false;
            atualizarSetoresAtivos();
            iniciar_pwm_buzzer(500);
            sleep_ms(50);
            parar_pwm_buzzer();
        }

        if (botao_a_press)
        {
            toggle_mode();
            botao_a_press = !botao_a_press;
        }

        cyw43_arch_poll();
        sleep_ms(100);
    }

    cyw43_arch_deinit();
    return 0;
}

// Funções existentes (mantidas sem alterações)
void gpio_led_bitdog(void)
{
    gpio_init(LED_BLUE_PIN);
    gpio_set_dir(LED_BLUE_PIN, GPIO_OUT);
    gpio_put(LED_BLUE_PIN, false);
    gpio_init(LED_GREEN_PIN);
    gpio_set_dir(LED_GREEN_PIN, GPIO_OUT);
    gpio_put(LED_GREEN_PIN, false);
    gpio_init(LED_RED_PIN);
    gpio_set_dir(LED_RED_PIN, GPIO_OUT);
    gpio_put(LED_RED_PIN, false);
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    tcp_recv(newpcb, tcp_server_recv);
    return ERR_OK;
}

void toggle_mode(void)
{
    if (strcmp(modo_atual, "Automatico") == 0)
    {
        modo_atual = "Manual";
        pararModoAutomatico();
    }
    else
    {
        Atualizar_Setor(1, 1);
        Atualizar_Setor(2, 0);
        Atualizar_Setor(3, 0);
        Atualizar_Setor(4, 0);
        setor_2comando = 0;
        setor_3comando = 0;
        setor_4comando = 0;
        setor_atual = 1;
        modo_atual = "Automatico";
        setor_1comando = 1;
        iniciarModoAutomatico();
        atualizarSetoresAtivos();
        iniciar_pwm_buzzer(500);
        sleep_ms(50);
        parar_pwm_buzzer();
    }
}

void user_request(char **request)
{
    if (strcmp(modo_atual, "Manual") == 0)
    {
        if (strstr(*request, "GET /Setor_1") != NULL)
        {
            setor_1comando = !setor_1comando;
            Atualizar_Setor(1, setor_1comando);
            atualizarSetoresAtivos();
            iniciar_pwm_buzzer(100);
            sleep_ms(100);
            parar_pwm_buzzer();
        }
        else if (strstr(*request, "GET /Setor_2") != NULL)
        {
            setor_2comando = !setor_2comando;
            Atualizar_Setor(2, setor_2comando);
            atualizarSetoresAtivos();
            iniciar_pwm_buzzer(100);
            sleep_ms(100);
            parar_pwm_buzzer();
        }
        else if (strstr(*request, "GET /Setor_3") != NULL)
        {
            setor_3comando = !setor_3comando;
            Atualizar_Setor(3, setor_3comando);
            atualizarSetoresAtivos();
            iniciar_pwm_buzzer(100);
            sleep_ms(100);
            parar_pwm_buzzer();
        }
        else if (strstr(*request, "GET /Setor_4") != NULL)
        {
            setor_4comando = !setor_4comando;
            Atualizar_Setor(4, setor_4comando);
            atualizarSetoresAtivos();
            iniciar_pwm_buzzer(100);
            sleep_ms(100);
            parar_pwm_buzzer();
        }
    }
    if (strstr(*request, "GET /toggle_mode") != NULL)
    {
        toggle_mode();
    }
}

float temp_read(void)
{
    adc_select_input(4);
    uint16_t raw_value = adc_read();
    const float conversion_factor = 3.3f / (1 << 12);
    float temperature = 27.0f - ((raw_value * conversion_factor) - 0.706f) / 0.001721f;
    return temperature;
}

static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if (!p)
    {
        tcp_close(tpcb);
        tcp_recv(tpcb, NULL);
        return ERR_OK;
    }
    char *request = (char *)malloc(p->len + 1);
    memcpy(request, p->payload, p->len);
    request[p->len] = '\0';
    printf("Request: %s\n", request);
    user_request(&request);
    float temperature = temp_read();
    char html[2048];
    snprintf(html, sizeof(html),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html; charset=UTF-8\r\n"
             "\r\n"
             "<!DOCTYPE html>\n"
             "<html>\n"
             "<head>\n"
             "<title>Sistema Irrigacao</title>\n"
             "<meta charset=\"UTF-8\">\n"
             "<style>\n"
             "body { font-family: Arial, sans-serif; text-align: center; margin-top: 50px; background-color: #f4f7f6; color: #333; }\n"
             "h1 { font-size: 48px; margin-bottom: 30px; color: #4CAF50; }\n"
             "button { background-color: #4CAF50; font-size: 24px; margin: 10px; padding: 10px 20px; border-radius: 10px; }\n"
             ".button-container { display: flex; justify-content: center; gap: 20px; margin-top: 30px; }\n"
             "</style>\n"
             "</head>\n"
             "<body>\n"
             "<h1>Irrigacao IoT</h1>\n"
             "<p class=\"mode\">Modo Atual: %s</p>\n"
             "<p class=\"Ligado\">Setores Ativos: %s</p>\n"
             "<form action=\"./toggle_mode\">\n"
             "<button>Alternar Modo</button>\n"
             "</form>\n"
             "<div class=\"button-container\">\n"
             "<form action=\"./Setor_1\"><button>Setor 1</button></form>\n"
             "<form action=\"./Setor_2\"><button>Setor 2</button></form>\n"
             "<form action=\"./Setor_3\"><button>Setor 3</button></form>\n"
             "<form action=\"./Setor_4\"><button>Setor 4</button></form>\n"
             "</div>\n"
             "</body>\n"
             "</html>\n",
             modo_atual, setores_ativos);
    tcp_write(tpcb, html, strlen(html), TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
    free(request);
    pbuf_free(p);
    return ERR_OK;
}

void iniciarModoAutomatico()
{
    strcpy(modo_atual, "Automatico");
    setor_atual = 1;
    if (!alarme_ativo)
    {
        add_repeating_timer_ms(10000, repeating_timer_callback, NULL, &meu_timer);
        alarme_ativo = true;
        printf("Modo automático ativado\n");
    }
}

void pararModoAutomatico()
{
    strcpy(modo_atual, "Manual");
    if (alarme_ativo)
    {
        cancel_repeating_timer(&meu_timer);
        alarme_ativo = false;
        printf("Modo automático desativado\n");
    }
}

void iniciar_pwm_buzzer(uint freq_hz)
{
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    uint clock = 125000000;
    uint divider = 4;
    uint top = clock / (divider * freq_hz);
    pwm_set_clkdiv(slice, divider);
    pwm_set_wrap(slice, top);
    pwm_set_chan_level(slice, pwm_gpio_to_channel(BUZZER_PIN), top / 2);
    pwm_set_enabled(slice, true);
}

void parar_pwm_buzzer()
{
    uint slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_set_enabled(slice, false);
}

void atualizarSetoresAtivos()
{
    setores_ativos[0] = '\0';
    if (setor_1comando == 1)
    {
        strcat(setores_ativos, "1");
    }
    if (setor_2comando == 1)
    {
        if (strlen(setores_ativos) > 0)
            strcat(setores_ativos, " - ");
        strcat(setores_ativos, "2");
    }
    if (setor_3comando == 1)
    {
        if (strlen(setores_ativos) > 0)
            strcat(setores_ativos, " - ");
        strcat(setores_ativos, "3");
    }
    if (setor_4comando == 1)
    {
        if (strlen(setores_ativos) > 0)
            strcat(setores_ativos, " - ");
        strcat(setores_ativos, "4");
    }
    if (strlen(setores_ativos) == 0)
    {
        strcpy(setores_ativos, "Nenhum");
    }
    printf("\n Setores Ativos: %s\n", setores_ativos);
}

void update_display(ssd1306_t *ssd)
{
    ssd1306_fill(ssd, false);
    int borda = 2;
    ssd1306_rect(ssd, 0, 0, 128, 60, true, false);
    ssd1306_draw_string(ssd, "Wifi ", 8 + borda, 18 + borda);
    ssd1306_draw_string(ssd, modo_atual, 5 + borda, 6 + borda);
    if (wifi_connected)
    {
        ssd1306_draw_string(ssd, "On", 50, 18 + borda);
    }
    else
    {
        ssd1306_draw_string(ssd, "Off", 50, 18 + borda);
    }
    if (setor_1comando)
    {
        ssd1306_rect(ssd, 20, 20 + 20 + 20 + 10 + 20, 10, 10, true, true);
    }
    else
    {
        ssd1306_rect(ssd, 20, 20 + 20 + 20 + 10 + 20, 10, 10, true, false);
    }
    if (setor_2comando)
    {
        ssd1306_rect(ssd, 20, 20 + 20 + 20 + 20 + 10 + 20, 10, 10, true, true);
    }
    else
    {
        ssd1306_rect(ssd, 20, 20 + 20 + 20 + 20 + 10 + 20, 10, 10, true, false);
    }
    if (setor_3comando)
    {
        ssd1306_rect(ssd, 40, 20 + 20 + 20 + 10 + 20, 10, 10, true, true);
    }
    else
    {
        ssd1306_rect(ssd, 40, 20 + 20 + 20 + 10 + 20, 10, 10, true, false);
    }
    if (setor_4comando)
    {
        ssd1306_rect(ssd, 40, 20 + 20 + 20 + 20 + 10 + 20, 10, 10, true, true);
    }
    else
    {
        ssd1306_rect(ssd, 40, 20 + 20 + 20 + 20 + 10 + 20, 10, 10, true, false);
    }
    ssd1306_send_data(ssd);
}