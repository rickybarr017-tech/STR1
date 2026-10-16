// demo_rtos.cpp : contém a função 'main'. A execução do programa começa e termina aqui.
//

#include <stdio.h>
#include <string.h>
#include <conio.h>
#include <time.h>

extern "C" {
#include <FreeRTOS.h>
#include <task.h>
#include <timers.h>
#include <semphr.h>
#include <queue.h>      // filas de mensagens (mailboxes)
#include <interface.h>
#include <interrupts.h>
}

#include "my_interaction_functions.h"

#define TAMANHO_REGIAO_1   8201
#define TAMANHO_REGIAO_2   29905
#define TAMANHO_REGIAO_3   7607

// Tempos (em ms; neste projeto 1 tick = 1 ms)
#define PERIODO_BOTOES_MS        20     // período de leitura dos botões
#define TEMPO_FIM_PULSOS_MS      1500   // sem pulsos durante este tempo -> sequência terminada
#define PERIODO_LUZ_NORMAL_MS    500    // pisca durante arrumação/dispensa (FR8, FR9)
#define PERIODO_LUZ_RAPIDO_MS    150    // pisca mais depressa em emergência (FR10)

// -------------------------------------------------------------
// ESTRUTURAS E VARIÁVEIS DO SISTEMA
// -------------------------------------------------------------

// Tipos de pedido que entram na fila.
// TODO o movimento é feito pela tarefa gestora -> só uma tarefa mexe nos motores.
typedef enum {
    PED_ARRUMAR,       // valor = ID 1..9            (FR3)
    PED_DISPENSAR,     // valor = família 1..3       (FR4)
    PED_IR_CELULA,     // valor = x*10 + z           (FR1.3)
    PED_PASSO_X,       // valor = +1 / -1            (FR1.2)
    PED_PASSO_Y,
    PED_PASSO_Z
} TipoPedido;

typedef struct {
    TipoPedido tipo;
    int valor;
} Pedido;

// Registo de um serviço de dispensa concluído (FR14)
typedef struct {
    int id;
    int familia;
    time_t data_dispensa;
    ULONGLONG duracao_ms;
} RegistoDispensa;

// O que a tarefa gestora está a fazer (usado pelas luzes)
typedef enum {
    MODO_LIVRE,
    MODO_A_ARRUMAR,
    MODO_A_DISPENSAR
} ModoOperacao;

#define MAX_HISTORICO 100
RegistoDispensa historico[MAX_HISTORICO];
int num_historico = 0;

static unsigned long ordem_arrumacao = 0;   // critério FIFO

// Célula do componente que está a ser dispensado (-1 = nenhum).
// Entre a escolha e a retirada fica indisponível para consultas/pedidos.
static int x_a_dispensar = -1, z_a_dispensar = -1;

static volatile int gestor_ocupado = 0;            // 1 enquanto a gestora executa um pedido
static volatile ModoOperacao modo = MODO_LIVRE;
static volatile int emergencia = 0;                // 1 durante um STOP (próximo passo)

xQueueHandle filaPedidos;          // fila de pedidos (NF1)
xSemaphoreHandle mutexArmazem;     // protege armazem, estatisticas, historico e x/z_a_dispensar

// -------------------------------------------------------------
// FUNÇÕES AUXILIARES
// -------------------------------------------------------------

static int familiaDe(int id)
{
    return ((id - 1) % 3) + 1;   // 1,4,7 -> 1 | 2,5,8 -> 2 | 3,6,9 -> 3
}

static void formatarData(time_t t, char* texto, size_t tamanho)
{
    struct tm info;
    localtime_s(&info, &t);      // versão segura (localtime dá erro C4996)
    strftime(texto, tamanho, "%d/%m/%Y %H:%M:%S", &info);
}

static void formatarDuracao(ULONGLONG ms, char* texto, size_t tamanho)
{
    unsigned long s = (unsigned long)(ms / 1000);
    snprintf(texto, tamanho, "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
}

static void mostrarPosicao()
{
    printf("\n[POSICAO] X=%d  Y=%d  Z=%d  (Z cima=%d)  (-1 = entre sensores)\n",
           obterPosX(), obterPosY(), obterPosZ(), obterPosZCima());
}

static void enviarPedido(TipoPedido tipo, int valor)
{
    Pedido p;
    p.tipo  = tipo;
    p.valor = valor;
    if (xQueueSend(filaPedidos, &p, 0) == pdTRUE)
        printf("\n[PEDIDO] Colocado na fila.\n");
    else
        printf("\n[PEDIDO] ERRO: fila cheia!\n");
}

// -------------------------------------------------------------
// FUNÇÕES DE SEGURANÇA E INICIALIZAÇÃO
// -------------------------------------------------------------

// Nome imposto pelo FreeRTOS (chamada quando um configASSERT falha)
void vAssertCalled(unsigned long ulLine, const char* const pcFileName) {
    printf("ASSERT! Linha %ld, ficheiro %s, GetLastError() %ld\r\n", ulLine, pcFileName, GetLastError());
    taskENTER_CRITICAL();
    while (1) {
        __asm { NOP };
        __asm { NOP };
    }
}

static void inicializarHeap(void) {
    static uint8_t ucHeap[configTOTAL_HEAP_SIZE];
    volatile uint32_t deslocamento_extra = 19;
    const HeapRegion_t regioes[] = {
        { ucHeap + 1, TAMANHO_REGIAO_1 },
        { ucHeap + 15 + TAMANHO_REGIAO_1, TAMANHO_REGIAO_2 },
        { ucHeap + 19 + TAMANHO_REGIAO_1 + TAMANHO_REGIAO_2, TAMANHO_REGIAO_3 },
        { NULL, 0 }
    };

    configASSERT((deslocamento_extra + TAMANHO_REGIAO_1 + TAMANHO_REGIAO_2 + TAMANHO_REGIAO_3) < configTOTAL_HEAP_SIZE);
    (void)deslocamento_extra;
    vPortDefineHeapRegions(regioes);
}

void inicializarPortos() {
    printf("\nA aguardar pelo simulador...");
    printf("\nLembrete: os movimentos automaticos exigem calibrar o kit primeiro...");
    createDigitalInput(0);
    createDigitalInput(1);
    createDigitalOutput(2);
    writeDigitalU8(2, 0);
    lerRepousoBotoes();          // botões largados no arranque
    printf("\nLigado ao simulador.\n");
}

// -------------------------------------------------------------
// OPERAÇÕES DE ALTO NÍVEL (executadas pela tarefa gestora)
// -------------------------------------------------------------

// FR3: arrumar o componente 'id' (que já está na cage, na entrada (1,1))
static void tratarArrumacao(int id)
{
    int fam = familiaDe(id);
    int dx = -1, dz = -1;
    int duplicado = 0;

    // 1. Validar e escolher célula NO INÍCIO DA EXECUÇÃO do pedido
    xSemaphoreTake(mutexArmazem, portMAX_DELAY);
    for (int x = 1; x <= 3; x++) {
        for (int z = 1; z <= 3; z++) {
            if (armazem[x][z].ocupada && armazem[x][z].id == id)
                duplicado = 1;
            if (!armazem[x][z].ocupada && dx == -1) {
                dx = x;
                dz = z;
            }
        }
    }
    xSemaphoreGive(mutexArmazem);

    // 2. Pedidos rejeitados não iniciam movimento
    if (duplicado) {
        printf("\n[GESTOR] ERRO: o componente %d ja esta guardado. Pedido rejeitado.\n", id);
        return;
    }
    if (dx == -1) {
        printf("\n[GESTOR] ERRO: armazem cheio. Componente %d nao arrumado.\n", id);
        return;
    }

    printf("\n[GESTOR] Arrumar componente %d (familia %d) na celula (%d,%d)...\n", id, fam, dx, dz);
    modo = MODO_A_ARRUMAR;           // luz de cima começa a piscar (FR8)

    // 3. Movimento: entrada -> célula -> pousar
    irParaY(2);                      // NF5: Y no centro antes de mexer X/Z
    irParaX(1);  irParaZ(1);         // cage na entrada, com o componente
    irParaX(dx); irParaZ(dz);
    porPecaNaCelula();

    // 4. Registar no fim da arrumação
    xSemaphoreTake(mutexArmazem, portMAX_DELAY);
    Celula* c = &armazem[dx][dz];
    c->ocupada        = 1;
    c->id             = id;
    c->familia        = fam;
    c->tick_arrumacao = GetTickCount64();
    c->data_arrumacao = time(NULL);
    c->ordem          = ++ordem_arrumacao;
    time_t data       = c->data_arrumacao;
    xSemaphoreGive(mutexArmazem);

    modo = MODO_LIVRE;

    char texto[32];
    formatarData(data, texto, sizeof(texto));
    printf("[GESTOR] Componente %d (familia %d) arrumado em (%d,%d) as %s\n", id, fam, dx, dz, texto);
}

// FR4: dispensar o componente mais antigo (FIFO) da família 'fam'
static void tratarDispensa(int fam)
{
    int ex = -1, ez = -1;
    Celula escolhida;

    // 1. Escolher o componente SÓ AGORA, quando o pedido começa (NF1)
    xSemaphoreTake(mutexArmazem, portMAX_DELAY);
    for (int x = 1; x <= 3; x++) {
        for (int z = 1; z <= 3; z++) {
            Celula* c = &armazem[x][z];
            if (c->ocupada && c->familia == fam) {
                if (ex == -1 || c->ordem < armazem[ex][ez].ordem) {
                    ex = x;
                    ez = z;
                }
            }
        }
    }
    if (ex != -1) {
        escolhida = armazem[ex][ez];
        x_a_dispensar = ex;          // fica indisponível a partir de agora
        z_a_dispensar = ez;
    }
    xSemaphoreGive(mutexArmazem);

    // 2. Sem stock: informar e não mexer a cage
    if (ex == -1) {
        printf("\n[GESTOR] Familia %d sem stock. Pedido terminado.\n", fam);
        return;
    }

    printf("\n[GESTOR] Dispensar familia %d: componente %d na celula (%d,%d)...\n", fam, escolhida.id, ex, ez);
    modo = MODO_A_DISPENSAR;         // luz de baixo começa a piscar (FR9)

    // 3. Movimento: célula -> retirar -> entrada/saída
    irParaY(2);
    irParaX(ex); irParaZ(ez);
    tirarPecaDaCelula();

    xSemaphoreTake(mutexArmazem, portMAX_DELAY);
    armazem[ex][ez].ocupada = 0;     // a célula fica vazia
    x_a_dispensar = z_a_dispensar = -1;
    xSemaphoreGive(mutexArmazem);

    irParaX(1); irParaZ(1);          // entrega em (1,1)

    // 4. Só conta o serviço depois da entrega
    ULONGLONG duracao = GetTickCount64() - escolhida.tick_arrumacao;

    xSemaphoreTake(mutexArmazem, portMAX_DELAY);
    if (num_historico < MAX_HISTORICO) {
        historico[num_historico].id            = escolhida.id;
        historico[num_historico].familia       = fam;
        historico[num_historico].data_dispensa = time(NULL);
        historico[num_historico].duracao_ms    = duracao;
        num_historico++;
    }
    estatisticas.dispensados_familia[fam]++;
    estatisticas.total_dispensados++;
    xSemaphoreGive(mutexArmazem);

    modo = MODO_LIVRE;

    char texto[16];
    formatarDuracao(duracao, texto, sizeof(texto));
    printf("[GESTOR] Entregue: componente %d (familia %d), tempo armazenado: %s\n", escolhida.id, fam, texto);
}

// -------------------------------------------------------------
// CONSULTAS (FR11 a FR15) - só leem dados, protegidas pelo mutex (NF2)
// -------------------------------------------------------------

// FR11: lista de componentes guardados
static void mostrarComponentesGuardados()
{
    char data[32], tempo[16];
    int n = 0;
    ULONGLONG agora = GetTickCount64();

    printf("\n--- COMPONENTES GUARDADOS (FR11) ---\n");
    printf(" ID  Fam  Celula  Arrumado a            Tempo guardado\n");

    xSemaphoreTake(mutexArmazem, portMAX_DELAY);
    for (int x = 1; x <= 3; x++) {
        for (int z = 1; z <= 3; z++) {
            Celula* c = &armazem[x][z];
            if (!c->ocupada) continue;
            formatarData(c->data_arrumacao, data, sizeof(data));
            formatarDuracao(agora - c->tick_arrumacao, tempo, sizeof(tempo));
            printf(" %2d   %d   (%d,%d)   %s   %s%s\n", c->id, c->familia, x, z, data, tempo,
                   (x == x_a_dispensar && z == z_a_dispensar) ? "  [a ser dispensado]" : "");
            n++;
        }
    }
    xSemaphoreGive(mutexArmazem);

    if (n == 0) printf(" (armazem vazio)\n");
}

// FR12: stock de uma família e próximo componente FIFO
static void mostrarFamilia(int fam)
{
    int quantos = 0, px = -1, pz = -1;
    char data[32];

    xSemaphoreTake(mutexArmazem, portMAX_DELAY);
    for (int x = 1; x <= 3; x++) {
        for (int z = 1; z <= 3; z++) {
            Celula* c = &armazem[x][z];
            if (!c->ocupada || c->familia != fam) continue;
            if (x == x_a_dispensar && z == z_a_dispensar) continue;   // já não está disponível
            quantos++;
            if (px == -1 || c->ordem < armazem[px][pz].ordem) {
                px = x;
                pz = z;
            }
        }
    }
    printf("\n--- FAMILIA %d (FR12) ---\n", fam);
    if (quantos == 0) {
        printf(" Sem stock.\n");
    }
    else {
        formatarData(armazem[px][pz].data_arrumacao, data, sizeof(data));
        printf(" Disponiveis: %d\n", quantos);
        printf(" Proximo a dispensar (FIFO): ID %d, celula (%d,%d), arrumado a %s\n",
               armazem[px][pz].id, px, pz, data);
    }
    xSemaphoreGive(mutexArmazem);
}

// FR13: consulta de uma célula
static void mostrarCelula(int x, int z)
{
    char data[32], tempo[16];

    xSemaphoreTake(mutexArmazem, portMAX_DELAY);
    Celula* c = &armazem[x][z];
    printf("\n--- CELULA (%d,%d) (FR13) ---\n", x, z);
    if (!c->ocupada) {
        printf(" Vazia.\n");
    }
    else {
        formatarData(c->data_arrumacao, data, sizeof(data));
        formatarDuracao(GetTickCount64() - c->tick_arrumacao, tempo, sizeof(tempo));
        printf(" ID %d, familia %d, arrumado a %s, tempo guardado %s%s\n",
               c->id, c->familia, data, tempo,
               (x == x_a_dispensar && z == z_a_dispensar) ? "  [a ser dispensado]" : "");
    }
    xSemaphoreGive(mutexArmazem);
}

// FR14: histórico de dispensas concluídas
static void mostrarHistorico()
{
    char data[32], tempo[16];

    printf("\n--- HISTORICO DE DISPENSAS (FR14) ---\n");
    xSemaphoreTake(mutexArmazem, portMAX_DELAY);
    if (num_historico == 0) printf(" (nenhuma dispensa concluida)\n");
    for (int i = 0; i < num_historico; i++) {
        formatarData(historico[i].data_dispensa, data, sizeof(data));
        formatarDuracao(historico[i].duracao_ms, tempo, sizeof(tempo));
        printf(" %2d) ID %d  Fam %d  Dispensado a %s  Tempo guardado %s\n",
               i + 1, historico[i].id, historico[i].familia, data, tempo);
    }
    xSemaphoreGive(mutexArmazem);
}

// FR15: contagem de dispensas por família e total
static void mostrarContagens()
{
    printf("\n--- DISPENSAS CONCLUIDAS (FR15) ---\n");
    xSemaphoreTake(mutexArmazem, portMAX_DELAY);
    for (int f = 1; f <= 3; f++)
        printf(" Familia %d: %d\n", f, estatisticas.dispensados_familia[f]);
    printf(" Total:     %d\n", estatisticas.total_dispensados);
    xSemaphoreGive(mutexArmazem);
}

// -------------------------------------------------------------
// TAREFAS
// -------------------------------------------------------------

// TAREFA 1: Gestor (executa os pedidos por ordem de chegada; único dono dos motores)
void vTarefaGestor(void* pvParametros) {
    Pedido p;

    while (1) {
        if (xQueueReceive(filaPedidos, &p, portMAX_DELAY) == pdTRUE) {
            gestor_ocupado = 1;
            switch (p.tipo) {
            case PED_ARRUMAR:
                tratarArrumacao(p.valor);
                break;
            case PED_DISPENSAR:
                tratarDispensa(p.valor);
                break;
            case PED_IR_CELULA:
                irParaY(2);
                irParaX(p.valor / 10);
                irParaZ(p.valor % 10);
                mostrarPosicao();
                break;
            case PED_PASSO_X:
                passoX(p.valor);
                mostrarPosicao();
                break;
            case PED_PASSO_Y:
                passoY(p.valor);
                mostrarPosicao();
                break;
            case PED_PASSO_Z:
                passoZ(p.valor);
                mostrarPosicao();
                break;
            }
            gestor_ocupado = 0;
        }
    }
}

// Espera por um dígito no teclado sem bloquear as outras tarefas (-1 se não for dígito)
static int esperarDigito()
{
    while (1) {
        if (_kbhit()) {
            int c = _getch();
            if (c >= '0' && c <= '9') {
                printf("%c", c);
                return c - '0';
            }
            return -1;
        }
        vTaskDelay(20);
    }
}

// Comandos manuais só são aceites com o armazém parado e sem pedidos pendentes
static int manualPermitido()
{
    if (gestor_ocupado || uxQueueMessagesWaiting(filaPedidos) > 0) {
        printf("\n[MENU] Comando manual recusado: o armazem esta a executar pedidos.\n");
        return 0;
    }
    return 1;
}

static void mostrarMenu()
{
    printf("\n================ MENU DO ARMAZEM ================\n");
    printf(" Pedidos (vao para a fila):\n");
    printf("   1 - Arrumar componente (ID 1-9)            FR3\n");
    printf("   2 - Dispensar familia (1-3)                FR4\n");
    printf("   (ou: pulsos no botao de cima = ID, no de baixo = familia)\n");
    printf(" Consultas (disponiveis a qualquer momento):\n");
    printf("   3 - Componentes guardados                  FR11\n");
    printf("   4 - Stock e proximo de uma familia         FR12\n");
    printf("   5 - Consultar celula (X,Z)                 FR13\n");
    printf("   6 - Historico de dispensas                 FR14\n");
    printf("   7 - Contagem de dispensas                  FR15\n");
    printf(" Manual (so com o armazem parado):\n");
    printf("   a/d - X esquerda/direita   w/x - Z cima/baixo\n");
    printf("   q/e - Y fora/dentro        g   - Ir para celula (X,Z)\n");
    printf("   s   - Parar todos os motores\n");
    printf("   p   - Mostrar posicao      h   - Mostrar este menu\n");
    printf("=================================================\n");
}

// TAREFA 2: Menu (lê o teclado; pedidos -> fila, consultas -> mostra logo)
void vTarefaMenu(void* pvParametros) {
    mostrarMenu();

    while (1) {
        if (_kbhit()) {
            int tecla = _getch();
            int v, x, z;

            switch (tecla) {
            // ---------- Pedidos ----------
            case '1':
                printf("\nID do componente (1-9): ");
                v = esperarDigito();
                if (v < 1 || v > 9) printf("\n[MENU] ID invalido. Pedido rejeitado.\n");
                else                enviarPedido(PED_ARRUMAR, v);
                break;
            case '2':
                printf("\nFamilia (1-3): ");
                v = esperarDigito();
                if (v < 1 || v > 3) printf("\n[MENU] Familia invalida. Pedido rejeitado.\n");
                else                enviarPedido(PED_DISPENSAR, v);
                break;

            // ---------- Consultas ----------
            case '3':
                mostrarComponentesGuardados();
                break;
            case '4':
                printf("\nFamilia (1-3): ");
                v = esperarDigito();
                if (v < 1 || v > 3) printf("\n[MENU] Familia invalida.\n");
                else                mostrarFamilia(v);
                break;
            case '5':
                printf("\nX (1-3): ");
                x = esperarDigito();
                printf("  Z (1-3): ");
                z = esperarDigito();
                if (x < 1 || x > 3 || z < 1 || z > 3) printf("\n[MENU] Celula invalida.\n");
                else                                  mostrarCelula(x, z);
                break;
            case '6':
                mostrarHistorico();
                break;
            case '7':
                mostrarContagens();
                break;

            // ---------- Manual ----------
            case 'a': if (manualPermitido()) enviarPedido(PED_PASSO_X, -1); break;
            case 'd': if (manualPermitido()) enviarPedido(PED_PASSO_X, +1); break;
            case 'w': if (manualPermitido()) enviarPedido(PED_PASSO_Z, +1); break;
            case 'x': if (manualPermitido()) enviarPedido(PED_PASSO_Z, -1); break;
            case 'q': if (manualPermitido()) enviarPedido(PED_PASSO_Y, -1); break;
            case 'e': if (manualPermitido()) enviarPedido(PED_PASSO_Y, +1); break;
            case 'g':
                if (!manualPermitido()) break;
                printf("\nX (1-3): ");
                x = esperarDigito();
                printf("  Z (1-3): ");
                z = esperarDigito();
                if (x < 1 || x > 3 || z < 1 || z > 3) printf("\n[MENU] Celula invalida.\n");
                else                                  enviarPedido(PED_IR_CELULA, x * 10 + z);
                break;
            case 's':
                pararX(); pararY(); pararZ();
                printf("\n[MENU] Motores parados.\n");
                break;
            case 'p':
                mostrarPosicao();
                break;
            case 'h':
                mostrarMenu();
                break;
            }
        }
        vTaskDelay(50);
    }
}

// Chamada quando se premem os dois botões ao mesmo tempo.
// O STOP de emergência (FR5) é o próximo passo; por agora só avisa.
static void pedirParagemEmergencia()
{
    printf("\n[BOTOES] Dois botoes premidos: STOP detetado (ainda por implementar).\n");
}

// TAREFA 3: Botões (FR1.7). Conta pulsos: botão de cima = ID (FR3), de baixo = família (FR4).
// Os dois ao mesmo tempo = pedido de STOP (FR5).
void vTarefaBotoes(void* pvParametros) {
    int cima_antes = 0, baixo_antes = 0;
    int pulsos_cima = 0, pulsos_baixo = 0;
    ULONGLONG t_cima = 0, t_baixo = 0;
    int dois_premidos = 0;   // 1 desde que os dois ficam premidos até serem ambos largados

    while (1) {
        int cima  = botaoCimaPremido();
        int baixo = botaoBaixoPremido();
        ULONGLONG agora = GetTickCount64();

        if (cima && baixo) {
            if (!dois_premidos) {
                dois_premidos = 1;
                pulsos_cima = pulsos_baixo = 0;     // cancela sequências em curso
                pedirParagemEmergencia();
            }
        }
        else if (dois_premidos) {
            if (!cima && !baixo) dois_premidos = 0;  // só volta ao normal depois de largar os dois
        }
        else {
            // Flanco de subida = novo pulso
            if (cima && !cima_antes) {
                pulsos_cima++;
                t_cima = agora;
                printf("\n[BOTOES] Botao de cima: %d pulso(s)", pulsos_cima);
            }
            if (baixo && !baixo_antes) {
                pulsos_baixo++;
                t_baixo = agora;
                printf("\n[BOTOES] Botao de baixo: %d pulso(s)", pulsos_baixo);
            }

            // Fim da sequência do botão de cima -> pedido de arrumação
            if (pulsos_cima > 0 && !cima && agora - t_cima > TEMPO_FIM_PULSOS_MS) {
                if (pulsos_cima <= 9) {
                    printf("\n[BOTOES] Arrumar componente ID %d (familia %d).",
                           pulsos_cima, familiaDe(pulsos_cima));
                    enviarPedido(PED_ARRUMAR, pulsos_cima);
                }
                else {
                    printf("\n[BOTOES] %d pulsos: ID invalido (1-9). Pedido rejeitado.\n", pulsos_cima);
                }
                pulsos_cima = 0;
            }

            // Fim da sequência do botão de baixo -> pedido de dispensa
            if (pulsos_baixo > 0 && !baixo && agora - t_baixo > TEMPO_FIM_PULSOS_MS) {
                if (pulsos_baixo <= 3) {
                    printf("\n[BOTOES] Dispensar familia %d.", pulsos_baixo);
                    enviarPedido(PED_DISPENSAR, pulsos_baixo);
                }
                else {
                    printf("\n[BOTOES] %d pulsos: familia invalida (1-3). Pedido rejeitado.\n", pulsos_baixo);
                }
                pulsos_baixo = 0;
            }
        }

        cima_antes  = cima;
        baixo_antes = baixo;
        vTaskDelay(PERIODO_BOTOES_MS);
    }
}

// TAREFA 4: Luzes (FR1.8). Cima pisca a arrumar (FR8), baixo pisca a dispensar (FR9),
// as duas piscam mais depressa em emergência (FR10).
void vTarefaLuzes(void* pvParametros) {
    int acesa = 0;

    while (1) {
        int periodo;

        if (emergencia) {
            acesa = !acesa;
            definirLuzCima(acesa);
            definirLuzBaixo(acesa);
            periodo = PERIODO_LUZ_RAPIDO_MS;
        }
        else if (modo == MODO_A_ARRUMAR) {
            acesa = !acesa;
            definirLuzCima(acesa);
            definirLuzBaixo(0);
            periodo = PERIODO_LUZ_NORMAL_MS;
        }
        else if (modo == MODO_A_DISPENSAR) {
            acesa = !acesa;
            definirLuzCima(0);
            definirLuzBaixo(acesa);
            periodo = PERIODO_LUZ_NORMAL_MS;
        }
        else {
            acesa = 0;
            definirLuzCima(0);
            definirLuzBaixo(0);
            periodo = 100;
        }
        vTaskDelay(periodo);
    }
}

// -------------------------------------------------------------
// HOOKS E MAIN
// -------------------------------------------------------------

void hookArranque(void) {
    inicializarPortos();

    filaPedidos  = xQueueCreate(10, sizeof(Pedido));
    mutexArmazem = xSemaphoreCreateMutex();

    xTaskCreate(vTarefaGestor, "Gestor", 150, NULL, 1, NULL);
    xTaskCreate(vTarefaMenu,   "Menu",   150, NULL, 1, NULL);
    xTaskCreate(vTarefaBotoes, "Botoes", 150, NULL, 2, NULL);   // prioridade maior: responde logo ao STOP
    xTaskCreate(vTarefaLuzes,  "Luzes",  150, NULL, 1, NULL);
}

void hookTick(void) {
    Sleep(0);
}

void hookInativo(void) {
    Sleep(0);
}

int main()
{
    inicializarHeap();

    // Ponteiros definidos pela biblioteca FreeRTOS fornecida (nomes não podem mudar)
    vApplicationDaemonTaskStartupHook = &hookArranque;
    vApplicationTickHook = &hookTick;
    vApplicationIdleHook = &hookInativo;

    vTaskStartScheduler();
    return 0;
}
