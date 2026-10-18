#include "my_interaction_functions.h"
#include <windows.h> //for Sleep function
#include <stdio.h>
#include <time.h>    // [Rodrigo] para a funcao time() (data real)
#include <FreeRTOS.h>
#include <timers.h>
#include "consultas.h"   // [Rodrigo] registarDispensa (FR14/FR15)

/* =====================================================================
   RESUMO DAS ALTERACOES (procurar pelas etiquetas no codigo, Ctrl+F)

   [FR5-FR10]  STOP / RESUME / RESET corrigidos e luzes (FR8, FR9, FR10)
   [Rodrigo]   ligacao as consultas FR11-FR15, data real da arrumacao,
               sensor de caixa na cage (P1.4)
   [NF1]       fila unica de pedidos: os botoes (e o teclado) so colocam
               pedidos na fila; a vTaskExecutaPedidos executa-os UM DE CADA
               VEZ, por ordem de chegada. O componente a dispensar so e
               escolhido quando o pedido comeca. Antes, a arrumacao e a
               dispensa corriam em tarefas diferentes (podiam mexer o kit ao
               mesmo tempo) e cliques feitos durante uma operacao somavam-se.
   [NF3]       rejeitar um ID que ja esteja guardado; IDs fora de 1-9 e
               familias fora de 1-3 sao rejeitados (antes eram "cortados")
   [NF4]       vTaskLimites: desliga o motor de um eixo quando chega ao
               ultimo sensor (protege tambem os comandos manuais do menu)
   [NF5]       gotoX/gotoZ so andam com o Y parado no centro (Y == 2)

   NOTA: no simulador, usar SPEED = 0.1 (com velocidades maiores as caixas
   nao ficam nas prateleiras).
   ===================================================================== */

// [Rodrigo] o struct CelulaArmazem passou para o .h

void piscaCima();
void piscaBaixo();
void piscarDuasLuzesFast();
void resetLuzes();
bool verificarSegurancaEAtualizarLuzes();   // [FR5-FR10] agora devolve false se houver RESET

// Matriz de células (índices 1 a 3)
CelulaArmazem armazem[4][4] = { { {0} } };   // [Rodrigo] tirado o static: as consultas tambem a leem

//variaveis para XZ
static xQueueHandle global_mbx_XZ = NULL;
static xSemaphoreHandle global_sem_XZ_finished = NULL;

xTaskHandle emergencyTask = NULL;
xTaskHandle resumeTask = NULL;
xTaskHandle taskSenderCoord = NULL; // handle de xz

static volatile int global_piscar_mode = 0; // 0 = desligado, 1 = cima , 2 = baixo
static volatile bool em_emergencia = false;

// [FR5-FR10] novos estados para o STOP / RESUME / RESET
static volatile bool cancelar_operacao = false;  // RESET: os movimentos em curso desistem
static volatile int  operacoes_em_curso = 0;     // arrumacoes/dispensas a decorrer
static volatile bool peca_na_cage = false;       // ha um componente dentro da cage
static volatile int  id_na_cage = 0;             // ID desse componente

// [NF1] fila unica de pedidos e componente entregue que ainda esta na cage
static xQueueHandle filaPedidos = NULL;
static volatile bool componente_entregue_na_cage = false;

// [FR5-FR10] contadores de cliques passaram para aqui (o STOP tem de os apagar)
volatile int contador_impulsos = 0;
volatile ULONGLONG ultimo_tempo_clique = 0;
volatile int contador_impulsos_baixo = 0;
volatile ULONGLONG ultimo_tempo_clique_baixo = 0;

// [FR5-FR10] contam quantas arrumacoes/dispensas estao a decorrer (o RESET espera por elas)
static void iniciarOperacao()  { taskENTER_CRITICAL(); operacoes_em_curso++; taskEXIT_CRITICAL(); }
static void terminarOperacao() { taskENTER_CRITICAL(); operacoes_em_curso--; taskEXIT_CRITICAL(); }

void setGotoXZHandles(xQueueHandle mbx, xSemaphoreHandle sem) {
    global_mbx_XZ = mbx;
    global_sem_XZ_finished = sem;
}

void pedirMovimentoXZ(int x, int z) {

    if (global_mbx_XZ != NULL && global_sem_XZ_finished != NULL) {
        CoordenadasXZ coords = { x, z };
        xQueueSend(global_mbx_XZ, &coords, portMAX_DELAY);

        // [FR5-FR10] espera SEMPRE pelo fim do movimento. Com RESET os eixos desistem
        // logo, por isso a espera e curta e nao ficam semaforos "atrasados" que
        // fariam o movimento seguinte terminar antes do tempo.
        while (xSemaphoreTake(global_sem_XZ_finished, (TickType_t)10) != pdTRUE) {
            verificarSegurancaEAtualizarLuzes();
        }
    }
    else {
        // caso nao esteja inicializado
        gotoX(x);
        gotoZ(z);
    }
}



int getBitValue(uInt8 value, uInt8 bit_n)
// given a byte value, returns the value of its bit n
{
    return(value & (1 << bit_n));
}


void setBitValue(uInt8* variable, int n_bit, int new_value_bit)// given a byte value, set the n bit to value
{
    uInt8 mask_on = (uInt8)(1 << n_bit);
    uInt8 mask_off = ~mask_on;
    if (new_value_bit) *variable |= mask_on;
    else *variable &= mask_off;
}


// [FR5-FR10] FR7: RESET
// 1) cancela a operacao em curso (todos os ciclos de movimento desistem e os motores param);
// 2) espera que as tarefas de arrumacao/dispensa terminem (antes, continuavam a mexer
//    os motores AO MESMO TEMPO que a calibracao - era este o erro do RESET);
// 3) calibra; 4) se havia componente na cage, fica em (1,1) para ser retirado.
// Os componentes guardados mantem-se, com a ordem FIFO e as datas originais
// (antes o removerBoxes() apagava o armazem todo, o que o enunciado nao permite).
static void fazerReset() {
    printf("\nA efetuar RESET do sistema\n");

    cancelar_operacao = true;
    em_emergencia = false;          // desbloqueia as tarefas para verem o cancelamento
    stopX();
    stopY();
    stopZ();

    // [NF1] "recomecar a sequencia do inicio": descarta os pedidos em espera e os cliques
    // (feito ja aqui para a vTaskExecutaPedidos nao comecar outro pedido durante o RESET)
    if (filaPedidos != NULL) xQueueReset(filaPedidos);
    contador_impulsos = 0;
    contador_impulsos_baixo = 0;

    // espera que as operacoes em curso desistam
    while (operacoes_em_curso > 0)
        vTaskDelay(10);
    vTaskDelay(200);                // margem para os movimentos do menu tambem terminarem
    if (filaPedidos != NULL) xQueueReset(filaPedidos);   // [NF1] por seguranca, outra vez

    cancelar_operacao = false;      // a partir daqui os motores podem voltar a andar
    global_piscar_mode = 0;
    resetLuzes();

    bool havia_peca = peca_na_cage;
    calibrarKit();                  // termina em (1,1), que e a entrada/saida

    if (havia_peca) {
        printf("O componente %d estava na cage: retire-o em (1,1).\n", id_na_cage);
        peca_na_cage = false;
        componente_entregue_na_cage = true;   // [NF1] o proximo pedido espera que o retirem
    }
    printf("RESET concluido. Pedidos em espera descartados. Os componentes guardados mantem-se.\n");
}

//o parar
// [FR5-FR10] A tarefa fica a piscar as luzes enquanto houver emergencia.
// Sai sozinha do ciclo quando ha RESUME (a vTaskResume limpa em_emergencia)
// ou quando faz o RESET. Ja nao e suspensa a meio por outra tarefa.
void vTaskEmergency(void* pvParameters) {
    for (;;) {
        vTaskSuspend(NULL); //para a si mesma ate ser acordado pelo ISR

        printf("\nPARAGEM DE EMERGENCIA\n");
        stopX();
        stopY();
        stopZ();
        resetLuzes();

        // [FR5-FR10] o botao de baixo pode ainda estar premido (foi usado no STOP):
        // so conta como RESET quando for largado e voltar a ser premido
        bool baixo_antes = true;

        // quando as luzes tem que estar a piscar mais rapido
        while (em_emergencia) {
            piscarDuasLuzesFast();   // FR10

            bool baixo = botaoBaixoPressionado();
            bool cima  = botaoCimaPressionado();

            if (baixo && !baixo_antes && !cima) {   // FR7
                fazerReset();
                break;
            }
            baixo_antes = baixo;
            vTaskDelay(10);
        }
        resetLuzes();
    }
}

//o continuar
// [FR5-FR10] Basta limpar a emergencia: as tarefas que estavam paradas dentro de
// verificarSegurancaEAtualizarLuzes() continuam exatamente onde estavam (FR6).
// Ja nao suspende/retoma outras tarefas (isso deixava a goto_XZ_task suspensa
// depois de um RESET e a tarefa de emergencia parada a meio do ciclo).
void vTaskResume(void* pvParameters) {
    for (;;) {
        vTaskSuspend(NULL); //para a si mesma ate ser acordado pelo ISR

        printf("\nA retomar operacao\n");
        em_emergencia = false;
    }
}

// funcao auxiliar para verificar emergencia e atualizar luzes
// [FR5-FR10] chamada em todos os ciclos de movimento:
//  - em STOP: mantem os motores parados e espera (FR5); com RESUME devolve true e o movimento continua (FR6)
//  - com RESET: para os motores e devolve false, para o movimento desistir (FR7)
//  - normal: faz piscar a luz da operacao em curso (FR8, FR9)
bool verificarSegurancaEAtualizarLuzes() {

    while (em_emergencia && !cancelar_operacao) {
        stopX();
        stopY();
        stopZ();
        vTaskDelay(50);
    }

    if (cancelar_operacao) {
        stopX();
        stopY();
        stopZ();
        return false;
    }

    if (global_piscar_mode == 1)
        piscaCima();
    if (global_piscar_mode == 2)
        piscaBaixo();
    return true;
}

// X axis related functions
// [FR5-FR10] secoes criticas: as tarefas X e Z escrevem no porto 2 ao mesmo tempo
// (condicao de corrida, slides 5-6 do Lab 4)

void moveXLeft()
{
    taskENTER_CRITICAL();
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 6, 1); // set bit 6 to high level
    setBitValue(&p, 7, 0); // set bit 7 to low level
    writeDigitalU8(2, p); // update port 2
    taskEXIT_CRITICAL();
}

void moveXRight()
{
    taskENTER_CRITICAL();
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 6, 0); // set bit 6 to low level
    setBitValue(&p, 7, 1); // set bit 7 to high level
    writeDigitalU8(2, p); // update port 2
    taskEXIT_CRITICAL();
}

void stopX()
{
    taskENTER_CRITICAL();
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 6, 0); // set bit 6 to low level
    setBitValue(&p, 7, 0); // set bit 7 to low level
    writeDigitalU8(2, p); // update port 2
    taskEXIT_CRITICAL();
}

int getXPos()
{
    uInt8 p = readDigitalU8(0);
    if (!getBitValue(p, 2))
        return 1;
    if (!getBitValue(p, 1))
        return 2;
    if (!getBitValue(p, 0))
        return 3;
    return(-1);
}

void gotoX(int x) {
    int x_pos = getXPos();

    if (x < 1 || x > 3)
        return;

    if (x == x_pos)
        return;

    // [NF5] o X so se pode mexer com o Y parado no centro
    if (getYPos() != 2) {
        printf("\n[NF5] Movimento em X recusado: o Y nao esta no centro.\n");
        return;
    }

    // se a posição for -1, move para a esquerda até encontrar um sensor
    if (x_pos == -1) {
        while (getXPos() == -1) { //  até atingir o primeiro sensor
            if (!verificarSegurancaEAtualizarLuzes()) break;   // [FR5-FR10] RESET
            moveXLeft();
            vTaskDelay(10);
        }
        stopX();
        if (cancelar_operacao) return;
        x_pos = getXPos(); // atualiza a posição
    }

    if (x_pos < x) {
        while (getXPos() != x) {
            if (!verificarSegurancaEAtualizarLuzes()) break;   // [FR5-FR10] RESET
            moveXRight();
            vTaskDelay(10);
        }
        stopX();
    }
    else if (x_pos > x) {
        while (getXPos() != x) {
            if (!verificarSegurancaEAtualizarLuzes()) break;   // [FR5-FR10] RESET
            moveXLeft();
            vTaskDelay(10);
        }
        stopX();
    }
}

// Y axis related functions
void moveYInside()
{
    taskENTER_CRITICAL();
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 4, 0); // set bit 4 to low level
    setBitValue(&p, 5, 1); // set bit 5 to high level
    writeDigitalU8(2, p); // update port 2
    taskEXIT_CRITICAL();
}

void moveYOutside()
{
    taskENTER_CRITICAL();
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 4, 1); // set bit 4 to high level
    setBitValue(&p, 5, 0); // set bit 5 to low level
    writeDigitalU8(2, p); // update port 2
    taskEXIT_CRITICAL();
}

void stopY() {
    taskENTER_CRITICAL();
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 4, 0); // set bit 4 to low level
    setBitValue(&p, 5, 0); // set bit 5 to low level
    writeDigitalU8(2, p); // update port 2
    taskEXIT_CRITICAL();
}

int getYPos()
{
    uInt8 p = readDigitalU8(0);
    if (!getBitValue(p, 5))
        return 1;
    if (!getBitValue(p, 4))
        return 2;
    if (!getBitValue(p, 3))
        return 3;
    return(-1);
}

void gotoY(int y) {
    int y_pos = getYPos();

    if (y < 1 || y > 3)
        return;

    if (y == y_pos)
        return;

    // se estiver entre sensores, move para fora até encontrar uma posição válida
    if (y_pos == -1) {
        while (getYPos() == -1) { //  até encontrar um sensor
            if (!verificarSegurancaEAtualizarLuzes()) break;   // [FR5-FR10] RESET
            moveYOutside();
            vTaskDelay(10);
        }
        stopY();
        if (cancelar_operacao) return;
        y_pos = getYPos(); // atualiza a posição
    }

    if (y_pos < y) {
        while (getYPos() != y) {
            if (!verificarSegurancaEAtualizarLuzes()) break;   // [FR5-FR10] RESET
            moveYInside();
            vTaskDelay(10);
        }
        stopY();
    }
    else if (y_pos > y) {
        while (getYPos() != y) {
            if (!verificarSegurancaEAtualizarLuzes()) break;   // [FR5-FR10] RESET
            moveYOutside();
            vTaskDelay(10);
        }
        stopY();
    }
}

// Z axis related functions

void moveZUp()
{
    taskENTER_CRITICAL();
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 2, 0); // set bit 2 to low level
    setBitValue(&p, 3, 1); // set bit 3 to high level
    writeDigitalU8(2, p); // update port 2
    taskEXIT_CRITICAL();
}

void moveZDown()
{
    taskENTER_CRITICAL();
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 2, 1); // set bit 2 to high level
    setBitValue(&p, 3, 0); // set bit 3 to low level
    writeDigitalU8(2, p); // update port 2
    taskEXIT_CRITICAL();
}

void stopZ() {
    taskENTER_CRITICAL();
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 2, 0); // set bit 2 to low level
    setBitValue(&p, 3, 0); // set bit 3 to low level
    writeDigitalU8(2, p); // update port 2
    taskEXIT_CRITICAL();
}

int getZPos()
{
    uInt8 p0 = readDigitalU8(0);
    uInt8 p1 = readDigitalU8(1);
    if (!getBitValue(p1, 3))
        return 1;
    if (!getBitValue(p1, 1))
        return 2;
    if (!getBitValue(p0, 7))
        return 3;

    return(-1);
}

void gotoZ(int z) {
    int z_pos = getZPos();

    if (z < 1 || z > 3)
        return;

    if (z == z_pos)
        return;

    // [NF5] o Z so se pode mexer com o Y parado no centro
    if (getYPos() != 2) {
        printf("\n[NF5] Movimento em Z recusado: o Y nao esta no centro.\n");
        return;
    }

    // se a posição for -1, desce até encontrar um sensor
    if (z_pos == -1) {
        while (getZPos() == -1) { //  até atingir o primeiro sensor
            if (!verificarSegurancaEAtualizarLuzes()) break;   // [FR5-FR10] RESET
            moveZDown();
            vTaskDelay(10);
        }
        stopZ();
        if (cancelar_operacao) return;
        z_pos = getZPos(); // atualiza a posição
    }

    if (z_pos < z) {
        while (getZPos() != z) {
            if (!verificarSegurancaEAtualizarLuzes()) break;   // [FR5-FR10] RESET
            moveZUp();
            vTaskDelay(10);
        }
        stopZ();
    }
    else if (z_pos > z) {
        while (getZPos() != z) {
            if (!verificarSegurancaEAtualizarLuzes()) break;   // [FR5-FR10] RESET
            moveZDown();
            vTaskDelay(10);
        }
        stopZ();
    }
}

int getZPosicaoCell()
{
    uInt8 p0 = readDigitalU8(0);
    uInt8 p1 = readDigitalU8(1);
    if (!getBitValue(p1, 2))
        return 1;
    if (!getBitValue(p1, 0))
        return 2;
    if (!getBitValue(p0, 6))
        return 3;

    return(-1);
}

// [NF5] Descida curta dentro da celula: do sensor de cima para o sensor de baixo do
// MESMO nivel, com o garfo esticado. Faz parte de pousar/levantar o componente,
// nao e uma deslocacao entre celulas, por isso nao passa pelo gotoZ (que exige Y == 2).
static void descerParaSensorBaixo() {
    while (getZPos() == -1) {
        if (!verificarSegurancaEAtualizarLuzes()) break;   // RESET
        moveZDown();
        vTaskDelay(10);
    }
    stopZ();
}

// [FR5-FR10] a luz que pisca passou a ser escolhida por quem chama
// (arrumacao = cima, dispensa = baixo), para a luz de baixo piscar
// durante a dispensa toda (FR9). Depois de cada passo verifica-se o RESET.
void putPartInCell(int x, int z)
{
    if (!estaCalibrado()) {
        printf("Kit nao calibrado.\n");
        return;
    }

    // usar os semaforos e mailboxes
    pedirMovimentoXZ(x, z);
    if (cancelar_operacao) return;

    // sobe ate ao sensor de cima da celula
    while (getZPosicaoCell() == -1) {
        if (!verificarSegurancaEAtualizarLuzes()) break;
        moveZUp();
        vTaskDelay(10);
    }
    stopZ();
    if (cancelar_operacao) return;

    gotoY(3);      // Entra lá
    if (cancelar_operacao) return;

    descerParaSensorBaixo();  // [NF5] desce: o componente fica pousado na prateleira
    if (cancelar_operacao) return;
    peca_na_cage = false;     // [FR5-FR10] a partir daqui o componente esta na celula

    gotoY(2);      // Sai de lá
}

void takePartFromCell(int x, int z)
{
    if (!estaCalibrado()) {
        printf("Kit nao calibrado.\n");
        return;
    }

    // usar os semaforos e mailboxes
    pedirMovimentoXZ(x, z);
    if (cancelar_operacao) return;

    gotoY(3);      // Entra na célula
    if (cancelar_operacao) return;

    // sobe: levanta o componente da prateleira
    while (getZPosicaoCell() == -1) {
        if (!verificarSegurancaEAtualizarLuzes()) break;
        moveZUp();
        vTaskDelay(10);
    }
    stopZ();
    if (cancelar_operacao) return;
    peca_na_cage = true;      // [FR5-FR10] a partir daqui o componente esta na cage

    gotoY(2);      // Sai da célula
    if (cancelar_operacao) return;

    descerParaSensorBaixo();  // volta ao sensor de baixo (ja com Y no centro)
}

// porto 1, Bit 5 - superior
bool botaoCimaPressionado() {
    uInt8 p1 = readDigitalU8(1);

    return getBitValue(p1, 5) != 0; // Retorna true se o botao estiver premido
}

// porto 1, Bit 6 - inferior
bool botaoBaixoPressionado() {
    uInt8 p1 = readDigitalU8(1);

    return getBitValue(p1, 6) != 0;
}

// [Rodrigo] porto 1, Bit 4 - sensor de caixa na cage (1 = ha caixa)
// (confirmado no codigo do simulador: cage_sensor -> P1.4)
bool caixaNaCage() {
    uInt8 p1 = readDigitalU8(1);

    return getBitValue(p1, 4) != 0;
}

// luz superior porto 2, bit 0
void AtivarLuzCima(bool state) {

    taskENTER_CRITICAL();

    uInt8 p2 = readDigitalU8(2);
    setBitValue(&p2, 0, state ? 1 : 0);  //se state for true mete-se a 1 para ligar a luz, senao 0
    writeDigitalU8(2, p2);

    taskEXIT_CRITICAL(); //porto 2 e preciso
}

// luz inferior porto 2, bit 1
void AtivarLuzBaixo(bool state) {
    taskENTER_CRITICAL();

    uInt8 p2 = readDigitalU8(2);
    setBitValue(&p2, 1, state ? 1 : 0);
    writeDigitalU8(2, p2);

    taskEXIT_CRITICAL();
}


static bool calibrado = false;

bool estaCalibrado() {
    return calibrado;
}

void calibrarKit() {

    printf("\nA iniciar calibracao \n");

    gotoY(2);      // primeiro o Y (o X e o Z so andam com Y no centro - NF5)
    gotoZ(1);
    gotoX(1);

    // [NF4] so fica calibrado se chegou mesmo a referencia
    calibrado = (getXPos() == 1 && getYPos() == 2 && getZPos() == 1);
    if (calibrado)
        printf("Calibrado com sucesso na posicao (X=1, Y=2, Z=1)\n");
    else
        printf("Calibracao falhou: a cage nao chegou a (X=1, Y=2, Z=1)\n");
}


// [FR5-FR10] o sistema esta a operar (em movimento)? (FR5: o STOP so vale nesse caso)
static bool sistemaEmMovimento() {
    return operacoes_em_curso > 0 || (readDigitalU8(2) & 0xFC) != 0;   // bits 2 a 7 = motores
}

// [FR5-FR10] FR5: pedido de STOP (os dois botoes premidos)
static void pedirStop() {
    // os cliques usados para fazer o STOP nao podem contar como pedidos
    contador_impulsos = 0;
    contador_impulsos_baixo = 0;

    if (em_emergencia) return;

    if (!sistemaEmMovimento()) {
        printf("\nSTOP ignorado: o sistema nao esta em movimento\n");
        return;
    }

    em_emergencia = true;
    if (emergencyTask != NULL) {
        xTaskResumeFromISR(emergencyTask);   // [FR5-FR10] dentro de uma ISR usa-se a versao FromISR (slide 31 do Lab 4)
    }
}


void switch1_rising_isr(ULONGLONG lastTime) {

    // botao superior
    if (botaoBaixoPressionado()) {
        pedirStop();                               // os dois premidos -> STOP (FR5)
    }

    else if (em_emergencia) {
        if (resumeTask != NULL) {
            xTaskResumeFromISR(resumeTask);        // em STOP -> RESUME (FR6)
        }
    }

    else {
        ULONGLONG tempo_atual = GetTickCount64();
        ultimo_tempo_clique = tempo_atual;
        contador_impulsos++;
        printf("\nTotal de cliques: %d", contador_impulsos);
    }
}


// cliques botao inferior
void switch2_rising_isr(ULONGLONG lastTime) {

    // em STOP o botao de baixo faz RESET, lido pela vTaskEmergency (FR7)
    if (em_emergencia) {
        return;
    }

    // verficar se os dois estao pressionados
    if (botaoCimaPressionado()) {
        pedirStop();                               // [FR5-FR10]
    }

    // apenas os cliques
    else {
        ultimo_tempo_clique_baixo = GetTickCount64();
        contador_impulsos_baixo++;
        printf("\nTotal de cliques: %d", contador_impulsos_baixo);
    }
}


// =====================================================================
// [NF1] FILA UNICA DE PEDIDOS
// =====================================================================

void iniciarFilaPedidos() {
    filaPedidos = xQueueCreate(20, sizeof(Pedido));
}

// Valida o valor e coloca o pedido no fim da fila (nao bloqueia quem chama).
// [NF3] valores fora do intervalo sao rejeitados e nao chegam a provocar movimento.
bool enviarPedido(TipoPedido tipo, int valor) {
    if (tipo == PEDIDO_ARRUMAR && (valor < 1 || valor > 9)) {
        printf("[NF3] ID %d fora do intervalo (1 a 9). Pedido rejeitado.\n", valor);
        return false;
    }
    if (tipo == PEDIDO_DISPENSAR && (valor < 1 || valor > 3)) {
        printf("[NF3] Familia %d fora do intervalo (1 a 3). Pedido rejeitado.\n", valor);
        return false;
    }
    if (filaPedidos == NULL)
        return false;

    Pedido p;
    p.tipo = tipo;
    p.valor = valor;

    // a mensagem e escrita ANTES de enviar: a vTaskExecutaPedidos tem mais prioridade
    // e comeca logo a executar, o que baralhava a ordem das mensagens na consola
    int a_frente = (int)uxQueueMessagesWaiting(filaPedidos) + (operacoes_em_curso > 0 ? 1 : 0);
    printf("[NF1] Pedido recebido: %s %d (pedidos a frente: %d)\n",
           tipo == PEDIDO_ARRUMAR ? "arrumar ID" : "dispensar familia", valor, a_frente);

    if (xQueueSend(filaPedidos, &p, 0) != pdTRUE) {
        printf("[NF1] Fila de pedidos cheia. Pedido rejeitado.\n");
        return false;
    }
    return true;
}

// Sistema parado: nenhum pedido em curso nem em espera, e sem emergencia.
// Usado pelo menu para so aceitar comandos manuais nesse caso (NF1, NF3).
bool sistemaLivre() {
    return operacoes_em_curso == 0 && !em_emergencia &&
           (filaPedidos == NULL || uxQueueMessagesWaiting(filaPedidos) == 0);
}

// Escreve a data/hora atual em texto "dd/mm/aaaa hh:mm:ss"
static void escreverDataAgora(char* texto, size_t tamanho) {
    time_t t = time(NULL);
    struct tm info;
    localtime_s(&info, &t);
    strftime(texto, tamanho, "%d/%m/%Y %H:%M:%S", &info);
}

// [NF1] Se o componente entregue numa dispensa anterior (ou deixado por um RESET)
// ainda esta na cage, espera que o operador o retire (botao takeFromCage).
static void esperarRetiradaDaCage() {
    if (componente_entregue_na_cage && caixaNaCage()) {
        printf("Retire da cage o componente entregue (botao takeFromCage)...\n");
        while (caixaNaCage()) {
            if (!verificarSegurancaEAtualizarLuzes()) return;   // RESET
            vTaskDelay(100);
        }
    }
    componente_entregue_na_cage = false;
}

// FR3: arrumar o componente id_peca (executado pela vTaskExecutaPedidos)
static void executarArrumacao(int id_peca)
{
    int familia = ((id_peca - 1) % 3) + 1;   // Familia 1 (1,4,7), familia 2 (2,5,8), familia 3 (3,6,9)
    printf("\n[NF1] A executar pedido: arrumar componente %d (familia %d)\n", id_peca, familia);

    // [NF3] rejeitar um ID que ja esteja guardado (verificado quando o pedido comeca)
    for (int x = 1; x <= 3; x++) {
        for (int z = 1; z <= 3; z++) {
            if (armazem[x][z].ocupado && armazem[x][z].id == id_peca) {
                printf("[NF3] O componente %d ja esta guardado em (%d,%d). Pedido rejeitado.\n", id_peca, x, z);
                return;   // nao ha movimento
            }
        }
    }

    // procurar a primeira celula vazia no armazem
    int target_x = -1;
    int target_z = -1;
    bool celula_encontrada = false;

    for (int x = 1; x <= 3; x++) {
        for (int z = 1; z <= 3; z++) {
            if (!armazem[x][z].ocupado) {
                target_x = x;
                target_z = z;
                celula_encontrada = true;
                break;
            }
        }
        if (celula_encontrada) break;
    }

    if (!celula_encontrada) {
        printf("Armazem cheio. Componente %d nao arrumado.\n", id_peca);
        return;   // nao ha movimento
    }

    printf("Celula vazia encontrada: X = %d, Z = %d\n", target_x, target_z);

    iniciarOperacao();
    global_piscar_mode = 1;    // FR8: luz de cima pisca durante a arrumacao

    esperarRetiradaDaCage();   // [NF1]

    // o componente entra pela cage em (1,1): ir para a entrada e esperar que esteja la
    if (!cancelar_operacao)
        pedirMovimentoXZ(1, 1);
    if (!cancelar_operacao && !caixaNaCage()) {
        printf("Coloque o componente %d na cage (botao Box)...\n", id_peca);
        while (!caixaNaCage()) {
            if (!verificarSegurancaEAtualizarLuzes()) break;   // RESET
            vTaskDelay(100);
        }
    }

    bool arrumacao_iniciada = false;
    if (!cancelar_operacao) {
        printf("A arrumar a peca\n");
        peca_na_cage = true;   // o componente esta na cage
        id_na_cage = id_peca;
        arrumacao_iniciada = true;
        putPartInCell(target_x, target_z);   // poe peca_na_cage = false quando o pousa
    }

    global_piscar_mode = 0;
    resetLuzes();

    if (!arrumacao_iniciada || peca_na_cage) {
        // [FR5-FR10] RESET antes de pousar: nao fica registado no armazem
        printf("Arrumacao do componente %d interrompida.\n", id_peca);
    }
    else {
        // registar o tempo de conclusao
        ULONGLONG tempo_conclusao = GetTickCount64();

        // atualizar o inventário
        armazem[target_x][target_z].id = id_peca;
        armazem[target_x][target_z].familia = familia;
        armazem[target_x][target_z].ocupado = true;
        armazem[target_x][target_z].tempo_storage = tempo_conclusao;
        armazem[target_x][target_z].data_storage = time(NULL);   // [Rodrigo] data real para FR11-FR13

        char data[32];
        escreverDataAgora(data, sizeof(data));
        printf("ID da peca: %d\n", id_peca);
        printf("Familia: %d\n", familia);
        printf("Celula selecionada: X=%d, Z=%d\n", target_x, target_z);
        printf("Arrumacao concluida a %s\n\n", data);
    }

    terminarOperacao();
}

// FR4: dispensar o componente mais antigo da familia (executado pela vTaskExecutaPedidos)
static void executarDispensa(int familia_pretendida)
{
    printf("\n[NF1] A executar pedido: dispensar familia %d\n", familia_pretendida);

    // Familia sem stock -> informar logo, sem mexer a cage
    bool existe = false;
    for (int x = 1; x <= 3; x++)
        for (int z = 1; z <= 3; z++)
            if (armazem[x][z].ocupado && armazem[x][z].familia == familia_pretendida)
                existe = true;
    if (!existe) {
        printf("Familia %d out of stock. Nao existem pecas disponiveis. Cage nao se moveu.\n", familia_pretendida);
        return;
    }

    iniciarOperacao();

    // [Rodrigo][NF1] para ir buscar outro componente a cage tem de estar vazia:
    // espera que o operador retire o que la estiver (botao takeFromCage)
    if (caixaNaCage()) {
        printf("Retire o componente da cage para continuar a dispensa (botao takeFromCage)...\n");
        while (caixaNaCage()) {
            if (!verificarSegurancaEAtualizarLuzes()) break;   // RESET
            vTaskDelay(100);
        }
        if (!cancelar_operacao) printf("Cage vazia. A continuar.\n");
    }
    componente_entregue_na_cage = false;

    if (cancelar_operacao) {
        terminarOperacao();
        return;
    }

    // [NF1] so agora, quando o pedido comeca mesmo, se escolhe o componente: o mais antigo (FIFO)
    int target_x = -1;
    int target_z = -1;
    ULONGLONG oldest_time = (ULONGLONG)-1; // valor maximo inicial

    for (int x = 1; x <= 3; x++) {
        for (int z = 1; z <= 3; z++) {
            if (armazem[x][z].ocupado && armazem[x][z].familia == familia_pretendida) {
                if (armazem[x][z].tempo_storage < oldest_time) { //tempo menor = mais antiga
                    oldest_time = armazem[x][z].tempo_storage;
                    target_x = x;
                    target_z = z;
                }
            }
        }
    }

    printf("Peca mais antiga encontrada em (X=%d, Z=%d)\n", target_x, target_z);

    int id_peca = armazem[target_x][target_z].id;
    ULONGLONG tempo_armazenamento = armazem[target_x][target_z].tempo_storage;

    global_piscar_mode = 2;    // FR9: luz de baixo pisca durante a dispensa toda
    peca_na_cage = false;
    id_na_cage = id_peca;

    // Retirar a peca
    takePartFromCell(target_x, target_z);

    if (!peca_na_cage) {
        // [FR5-FR10] RESET antes de levantar: o componente continua na celula
        printf("Dispensa interrompida: o componente %d continua na celula.\n", id_peca);
    }
    else {
        // Marcar célula como desocupada (o componente ja saiu dela)
        armazem[target_x][target_z].ocupado = false;

        // Entregar em (1,1): a cage leva o componente ate a entrada/saida.
        // [FR5-FR10] antes era putPartInCell(1,1), que guardava o componente
        // NA CELULA (1,1) e punha a luz de cima a piscar em vez da de baixo (FR9)
        pedirMovimentoXZ(1, 1);

        if (cancelar_operacao) {
            printf("Dispensa interrompida: o componente %d esta na cage.\n", id_peca);
        }
        else {
            peca_na_cage = false;                 // entregue
            componente_entregue_na_cage = true;   // [NF1] o proximo pedido espera que o retirem

            // o tempo pretendido
            ULONGLONG duracao = GetTickCount64() - tempo_armazenamento;

            registarDispensa(id_peca, familia_pretendida, tempo_armazenamento);   // [Rodrigo] FR14/FR15

            printf("Peca dispensada com sucesso\n");
            printf("ID da Peca: %d\n", id_peca);
            printf("Familia: %d\n", familia_pretendida);
            printf("Duracao do armazenamento: %llu ms\n\n", duracao);
        }
    }

    global_piscar_mode = 0;
    resetLuzes();
    terminarOperacao();
}

// [NF1] Unica tarefa que executa arrumacoes e dispensas: um pedido de cada vez,
// pela ordem em que entraram na fila. Assim o kit nunca faz duas operacoes ao mesmo tempo.
void vTaskExecutaPedidos(void* pvParameters) {
    Pedido p;
    for (;;) {
        if (xQueueReceive(filaPedidos, &p, portMAX_DELAY) == pdTRUE) {
            if (!estaCalibrado()) {
                printf("\nKit nao calibrado (prime 'c'). Pedido rejeitado.\n");
                continue;
            }
            if (p.tipo == PEDIDO_ARRUMAR)
                executarArrumacao(p.valor);
            else
                executarDispensa(p.valor);
        }
    }
}

// [NF1] Esta tarefa so conta os cliques do botao de cima. No fim da sequencia
// (2 s sem cliques) coloca um pedido na fila; ja nao arruma diretamente.
// Assim, cliques feitos durante outra operacao viram pedidos separados.
void vTaskGereBotoes(void* pvParameters) {

    while (1) {
        // Se houve impulsos e ja passaram pelo menos 2 segundos desde o ultimo clique
        if (contador_impulsos > 0 && (GetTickCount64() - ultimo_tempo_clique) > 2000) {
            int id_peca = contador_impulsos;
            contador_impulsos = 0; // Reset para o próximo ciclo, ja foi guardado no id_peca

            printf("\nContagem terminada. ID da peca: %d\n", id_peca);
            enviarPedido(PEDIDO_ARRUMAR, id_peca);   // [NF3] rejeita IDs fora de 1 a 9
        }

        vTaskDelay(100); // Dá tempo de CPU a outras tarefas
    }
}

// [NF1] Igual, para o botao de baixo: no fim da sequencia coloca um pedido de dispensa na fila.
void vTaskGereDispensa(void* pvParameters) {
    while (1) {
        // depois de dois segundos parou de clicar
        if (contador_impulsos_baixo > 0 && (GetTickCount64() - ultimo_tempo_clique_baixo) > 2000) {
            int familia_pretendida = contador_impulsos_baixo;
            contador_impulsos_baixo = 0; // Reset

            printf("\ncontagem de dispensa terminada\n");
            printf("Familia solicitada: %d\n", familia_pretendida);
            enviarPedido(PEDIDO_DISPENSAR, familia_pretendida);   // [NF3] rejeita familias fora de 1 a 3
        }
        vTaskDelay(100);
    }
}


// =====================================================================
// [NF4] TAREFA DE SEGURANCA DOS LIMITES
// Se um motor estiver a empurrar um eixo para la do ultimo sensor, desliga-o.
// Protege os movimentos automaticos e sobretudo os comandos manuais do menu
// (a, d, w, s, u, i), que ligam o motor e so paravam com a tecla x.
// =====================================================================
void vTaskLimites(void* pvParameters) {
    for (;;) {
        uInt8 p0 = readDigitalU8(0);
        uInt8 p1 = readDigitalU8(1);
        uInt8 p2 = readDigitalU8(2);

        // X: esquerda (P2.6) com X=1 (P0.2 ativo a 0) / direita (P2.7) com X=3 (P0.0)
        if (getBitValue(p2, 6) && !getBitValue(p0, 2)) stopX();
        if (getBitValue(p2, 7) && !getBitValue(p0, 0)) stopX();

        // Y: fora (P2.4) com Y=1 (P0.5) / dentro (P2.5) com Y=3 (P0.3)
        if (getBitValue(p2, 4) && !getBitValue(p0, 5)) stopY();
        if (getBitValue(p2, 5) && !getBitValue(p0, 3)) stopY();

        // Z: a descer (P2.2) no sensor de baixo do nivel 1 (P1.3)
        //    a subir (P2.3) no sensor de cima do nivel 3 (P0.6)
        if (getBitValue(p2, 2) && !getBitValue(p1, 3)) stopZ();
        if (getBitValue(p2, 3) && !getBitValue(p0, 6)) stopZ();

        vTaskDelay(5);
    }
}


void goto_X_task(void* pvParameters) {
    int x;
    TaskGotoXParams* my_params = (TaskGotoXParams*)pvParameters;
    xQueueHandle mbx_X = my_params->mbx_X;
    xSemaphoreHandle sem_X_finished = my_params->sem_X_finished;

    while (TRUE) {
        if (xQueueReceive(mbx_X, &x, portMAX_DELAY) == pdTRUE) {
            gotoX(x);
            xSemaphoreGive(sem_X_finished); // Sinaliza que o eixo x terminou
        }
    }
}


void goto_Z_task(void* pvParameters) {
    int z;
    TaskGotoZParams* my_params = (TaskGotoZParams*)pvParameters;
    xQueueHandle mbx_Z = my_params->mbx_Z;
    xSemaphoreHandle sem_Z_finished = my_params->sem_Z_finished;

    while (TRUE) {
        if (xQueueReceive(mbx_Z, &z, portMAX_DELAY) == pdTRUE) {
            gotoZ(z);
            xSemaphoreGive(sem_Z_finished); // Sinaliza que o eixo Z terminou
        }
    }
}

//coordenador

void goto_XZ_task(void* pvParameters) {
    CoordenadasXZ coords;
    TaskGotoXZParams* my_params = (TaskGotoXZParams*)pvParameters;

    xQueueHandle mbx_XZ = my_params->mbx_XZ;

    // acesso as mailboxes e semaforos dos outros
    xQueueHandle mbx_X = my_params->params_X->mbx_X;
    xSemaphoreHandle sem_X_finished = my_params->params_X->sem_X_finished;

    xQueueHandle mbx_Z = my_params->params_Z->mbx_Z;
    xSemaphoreHandle sem_Z_finished = my_params->params_Z->sem_Z_finished;

    while (TRUE) {
        // espera a recepção de um pedido de coordenadas x e z
        if (xQueueReceive(mbx_XZ, &coords, portMAX_DELAY) == pdTRUE) {

            // envia a coordenada x para a mailbox do x
            xQueueSend(mbx_X, &coords.x, portMAX_DELAY);

            //envia a coordenada z para a mailbox do z
            xQueueSend(mbx_Z, &coords.z, portMAX_DELAY);

            // aguarda que ambos os eixos terminem o movimento
            xSemaphoreTake(sem_X_finished, portMAX_DELAY);
            xSemaphoreTake(sem_Z_finished, portMAX_DELAY);

            // se houver um semaforo de conclusão xz, dá o sinal
            if (my_params->sem_XZ_finished != NULL) {
                xSemaphoreGive(my_params->sem_XZ_finished);
            }
        }
    }
}

static ULONGLONG ultimo_tempo_cima = 0;
static bool estado_cima = false;

static ULONGLONG ultimo_tempo_baixo = 0;
static bool estado_baixo = false;

static ULONGLONG ultimo_tempo_fast = 0;
static bool estado_fast = false;

void resetLuzes() {
    ultimo_tempo_cima = 0;
    estado_cima = false;
    ultimo_tempo_baixo = 0;
    estado_baixo = false;
    ultimo_tempo_fast = 0;
    estado_fast = false;
    AtivarLuzCima(false);
    AtivarLuzBaixo(false);
}

// Funções de Luzes
void piscaCima() {
    ULONGLONG tempo_atual = GetTickCount64();
    if ((tempo_atual - ultimo_tempo_cima) >= 250) {
        estado_cima = !estado_cima;
        AtivarLuzCima(estado_cima);
        ultimo_tempo_cima = tempo_atual;
    }
}

void piscaBaixo() {
    ULONGLONG tempo_atual = GetTickCount64();
    if ((tempo_atual - ultimo_tempo_baixo) >= 250) {
        estado_baixo = !estado_baixo;
        AtivarLuzBaixo(estado_baixo);
        ultimo_tempo_baixo = tempo_atual;
    }
}

// [FR5-FR10] FR10: as duas luzes piscam juntas e mais depressa (antes alternavam)
void piscarDuasLuzesFast() {
    ULONGLONG tempo_atual = GetTickCount64();
    if ((tempo_atual - ultimo_tempo_fast) >= 100) {
        estado_fast = !estado_fast;
        AtivarLuzCima(estado_fast);
        AtivarLuzBaixo(estado_fast);
        ultimo_tempo_fast = tempo_atual;
    }
}

// [FR5-FR10] removerBoxes() foi apagada: o RESET ja nao apaga o armazem
// (o enunciado diz que os componentes guardados mantem a ordem FIFO e as datas)
