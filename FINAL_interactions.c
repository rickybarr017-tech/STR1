
#include "my_interaction_functions.h"
#include <windows.h> //for Sleep function
#include <stdio.h>
#include <time.h>    
#include <FreeRTOS.h>
#include <timers.h>
#include "consultas.h"   


void piscaCima();
void piscaBaixo();
void piscarDuasLuzesFast();
void resetLuzes();
bool verificarSegurancaEAtualizarLuzes();   // devolve false se houver reset

// matriz de celulas 
CelulaArmazem armazem[4][4] = { { {0} } };   

//variaveis para XZ
static xQueueHandle global_mbx_XZ = NULL;
static xSemaphoreHandle global_sem_XZ_finished = NULL;

xTaskHandle emergencyTask = NULL;
xTaskHandle resumeTask = NULL;
xTaskHandle taskSenderCoord = NULL; // handle de xz

static volatile int global_piscar_mode = 0; // 0 = desligado, 1 = cima , 2 = baixo
static volatile bool em_emergencia = false;

// estados para o stop, resume e reset
static volatile bool cancelar_operacao = false;  // os movimentos em curso desistem - reset
static volatile int  operacoes_em_curso = 0;     // arrumacoes ou dispensas a acontecer
static volatile bool peca_na_cage = false;       // ha uma caixa dentro da cage
static volatile int  id_na_cage = 0;             // id da caixa


static xQueueHandle filaPedidos = NULL;
static volatile bool caixa_entregue_na_cage = false;

//contadores de cliques
volatile int contador_impulsos = 0;
volatile ULONGLONG ultimo_tempo_clique = 0;
volatile int contador_impulsos_baixo = 0;
volatile ULONGLONG ultimo_tempo_clique_baixo = 0;

// contam quantas arrumacoes ou dispensas estao a decorrer -------------------------------- ver o pq do task
static void iniciarOperacao() {
    taskENTER_CRITICAL();
    operacoes_em_curso++;
    taskEXIT_CRITICAL();
}

static void terminarOperacao() {
    taskENTER_CRITICAL();
    operacoes_em_curso--;
    taskEXIT_CRITICAL(); 
}

void setGotoXZHandles(xQueueHandle mbx, xSemaphoreHandle sem) {
    global_mbx_XZ = mbx;
    global_sem_XZ_finished = sem;
}

void pedirMovimentoXZ(int x, int z) {

    if (global_mbx_XZ != NULL && global_sem_XZ_finished != NULL) {
        CoordenadasXZ coords = { x, z };
        xQueueSend(global_mbx_XZ, &coords, portMAX_DELAY);

        // espera sempre pelo fim do movimento (no caso de haver resume). Com RESET os eixos desistem
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


// 1) cancela a operacao em curso 
// 2) espera que as tarefas de arrumacao/dispensa "terminem"
// 3) calibra; 4) se havia caixa na cage, fica em (1,1) para ser retirado
static void fazerReset() {

    printf("\nA efetuar RESET do sistema\n");

    cancelar_operacao = true;
    em_emergencia = false;          // desbloqueia as tarefas 
    stopX();
    stopY();
    stopZ();

    if (filaPedidos != NULL)   // recomecar do inicio depois de um reset
        xQueueReset(filaPedidos);

    contador_impulsos = 0;
    contador_impulsos_baixo = 0;

    // espera que as operacoes em curso desistam
    while (operacoes_em_curso > 0)
        vTaskDelay(10);
    
    vTaskDelay(200);                // margem para os movimentos do menu tambem acabarem

    cancelar_operacao = false;      // agr os "motores" podem voltar a funcionar
    global_piscar_mode = 0;
    resetLuzes();

    bool havia_peca = peca_na_cage;
    calibrarKit();                  // termina no default

    if (havia_peca) {
        printf("A caixa %d estava na cage: retire-o em (1,1)\n", id_na_cage);
        peca_na_cage = false;
        caixa_entregue_na_cage = true;  // o proximo pedido espera que o retirem
    }
    printf("RESET concluido\n");
}


// a tarefa fica a piscar as luzes enquanto houver emergencia
// sai sozinha do ciclo quando ha resume, em_emergencia e limpado
void vTaskEmergency(void* pvParameters) {

    for (;;) {
        vTaskSuspend(NULL); //para a si mesma ate ser acordado pelo ISR

        printf("\nPARAGEM DE EMERGENCIA\n");
        stopX();
        stopY();
        stopZ();
        resetLuzes();

        // o botao de baixo pode ainda estar premido quando foi usado no stop
        // so conta como reset quando for largado e voltar a ser premido
        bool baixo_antes = true;

        // quando as luzes tem que estar a piscar mais rapido
        while (em_emergencia) {
            piscarDuasLuzesFast();  

            bool baixo = botaoBaixoPressionado();
            bool cima = botaoCimaPressionado();

            if (baixo && !baixo_antes && !cima) {   // faz o reset
                fazerReset();
                break;
            }
            baixo_antes = baixo;
            vTaskDelay(10);
        }
        resetLuzes();
    }
}

// basta limpar a emergencia, as tarefas que estavam paradas dentro de verificarSegurancaEAtualizarLuzes() continuam no mm sitio
void vTaskResume(void* pvParameters) {

    for (;;) {
        vTaskSuspend(NULL); //para a si mesma ate ser acordado pelo ISR

        printf("\nA retomar operacao\n");
        em_emergencia = false;
    }
}

// funcao auxiliar para verificar emergencia e atualizar luzes
bool verificarSegurancaEAtualizarLuzes() {

    while (em_emergencia && !cancelar_operacao) {  //stop
        stopX();
        stopY();
        stopZ();
        vTaskDelay(50);
    }

    if (cancelar_operacao) {  // reset - para os "motores" e devolve false, para desistir
        stopX();
        stopY();
        stopZ();
        return false;
    }


    if (global_piscar_mode == 1)    // faz piscar a luz
        piscaCima();
    if (global_piscar_mode == 2)
        piscaBaixo();
    
    return true;
}

// X
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


    //o X so se pode mexer com o Y parado no meio
    if (getYPos() != 2) {
        printf("\nMovimento em X recusado, o Y nao esta dentro da cage\n");
        return;
    }

    // se a posição for -1, move para a esquerda até encontrar um sensor
    if (x_pos == -1) {
        while (getXPos() == -1) { //  até atingir o primeiro sensor
            if (!verificarSegurancaEAtualizarLuzes()) break;   // caso de reset e luzes
            moveXLeft();
            vTaskDelay(10);
        }
        stopX();

        if (cancelar_operacao) return;
        x_pos = getXPos(); // pa atualizar a posição
    }

    if (x_pos < x) {
        while (getXPos() != x) {
            if (!verificarSegurancaEAtualizarLuzes()) break;   // caso de reset e luzes
            moveXRight();
            vTaskDelay(10);
        }
        stopX();
    }

    else if (x_pos > x) {
        while (getXPos() != x) {
            if (!verificarSegurancaEAtualizarLuzes()) break;   // caso de reset e luzes
            moveXLeft();
            vTaskDelay(10);
        }
        stopX();
    }
}

// Y
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
            if (!verificarSegurancaEAtualizarLuzes()) break;  // caso de reset e luzes
            moveYOutside();
            vTaskDelay(10);
        }
        stopY();

        if (cancelar_operacao) return;
        y_pos = getYPos(); // pa atualizar a posição
    }

    if (y_pos < y) {
        while (getYPos() != y) {
            if (!verificarSegurancaEAtualizarLuzes()) break;  // caso de reset e luzes
            moveYInside();
            vTaskDelay(10);
        }
        stopY();
    }

    else if (y_pos > y) {
        while (getYPos() != y) {
            if (!verificarSegurancaEAtualizarLuzes()) break;   // caso de reset e luzes
            moveYOutside();
            vTaskDelay(10);
        }
        stopY();
    }
}

// Z
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

    //o Z so se pode mexer com o Y parado no meio
    if (getYPos() != 2) {
        printf("\nMovimento em Z recusado, o Y nao esta dentro da cage\n");
        return;
    }

    // se a posição for -1, desce até encontrar um sensor
    if (z_pos == -1) {
        while (getZPos() == -1) { //  até atingir o primeiro sensor
            if (!verificarSegurancaEAtualizarLuzes()) break;   // caso de reset e luzes
            moveZDown();
            vTaskDelay(10);
        }
        stopZ();

        if (cancelar_operacao) return;
        z_pos = getZPos(); // pa atualizar a posição
    }

    if (z_pos < z) {
        while (getZPos() != z) {
            if (!verificarSegurancaEAtualizarLuzes()) break;   // caso de reset e luzes
            moveZUp();
            vTaskDelay(10);
        }
        stopZ();
    }

    else if (z_pos > z) {
        while (getZPos() != z) {
            if (!verificarSegurancaEAtualizarLuzes()) break;   // caso de reset e luzes
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

//ja que metemos o andar no X e no Z so quando Y = 2, temos q fazer uma funcao no caso de nao ser, para ir po sensor de baixo
static void descerParaSensorBaixo() {
    while (getZPos() == -1) {
        if (!verificarSegurancaEAtualizarLuzes()) break;   // nao esquecer do reset
        moveZDown();
        vTaskDelay(10);
    }
    stopZ();
}


// garantir se as luzes tem q estar a piscar quando usamos o botao h -----------------------------------------------
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

    gotoY(3);      // entra la
    if (cancelar_operacao) return;

    descerParaSensorBaixo();   // desce
    if (cancelar_operacao) return;
    peca_na_cage = false;     // caixa na celula

    gotoY(2);      // sai de la
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

    gotoY(3);      // entra na celula
    if (cancelar_operacao) return;

    // levanta a caixa daquela "prateleira"
    while (getZPosicaoCell() == -1) {
        if (!verificarSegurancaEAtualizarLuzes()) break;
        moveZUp();
        vTaskDelay(10);
    }
    stopZ();

    if (cancelar_operacao) return;
    peca_na_cage = true;      // esta na cage

    gotoY(2);      // sai da celula
    if (cancelar_operacao) return;

    descerParaSensorBaixo();   // volta ao sensor de baixo
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

    gotoY(2);
    pedirMovimentoXZ(1, 1);

    calibrado = (getXPos() == 1 && getYPos() == 2 && getZPos() == 1); // so esta calibrado quando chega nos limites

    if (calibrado)
        printf("Calibrado com sucesso na posicao (X=1, Y=2, Z=1)\n");
    else
        printf("Calibracao falhou: a cage nao chegou a (X=1, Y=2, Z=1)\n");
}


//o stop so conta se estiver em movimento
static bool sistemaEmMovimento() {
    return operacoes_em_curso > 0 || (readDigitalU8(2) & 0xFC) != 0;   // bits de 2 a 7
}

// os dois botaos premidos
static void pedirStop() {
    // os cliques usados para fazer o stop nao podem contar como pedidos
    contador_impulsos = 0;
    contador_impulsos_baixo = 0;

    if (em_emergencia) return;

    if (!sistemaEmMovimento()) {
        printf("\nO sistema nao esta em movimento\n");
        return;
    }

    em_emergencia = true;
    if (emergencyTask != NULL) {
        xTaskResumeFromISR(emergencyTask);   // esta dentro de uma ISR
    }
}


void switch1_rising_isr(ULONGLONG lastTime) {

    // botao superior
    if (botaoBaixoPressionado()) {
        pedirStop();                               // os dois premidos - stop 
    }

    else if (em_emergencia) {
        if (resumeTask != NULL) {
            xTaskResumeFromISR(resumeTask);        // de stop pa resume
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

    // em stop o botao de baixo faz reset, lido pela vTaskEmergency 
    if (em_emergencia) {
        return;
    }

    // verficar se os dois estao pressionados
    if (botaoCimaPressionado()) {
        pedirStop();
    }

    // apenas os cliques
    else {
        ultimo_tempo_clique_baixo = GetTickCount64();
        contador_impulsos_baixo++;
        printf("\nTotal de cliques: %d\n", contador_impulsos_baixo);
    }
}

//NF1
void iniciarFilaPedidos() {
    filaPedidos = xQueueCreate(20, sizeof(Pedido));
}


bool enviarPedido(TipoPedido tipo, int valor) {

    if (tipo == PEDIDO_ARRUMAR && (valor < 1 || valor > 9)) {
        printf("id %d fora do intervalo (1 a 9), pedido foi rejeitado\n", valor);
        return false;
    }
    if (tipo == PEDIDO_DISPENSAR && (valor < 1 || valor > 3)) {
        printf("Familia %d fora do intervalo (1 a 3), pedido foi rejeitado\n", valor);
        return false;
    }

    if (filaPedidos == NULL)
        return false;

    Pedido p;
    p.tipo = tipo;
    p.valor = valor;

    // a mensagem tem q ser escrita antes de enviar pq vTaskExecutaPedidos tem mais prioridade, baralhava aquilo
    int a_frente = (int)uxQueueMessagesWaiting(filaPedidos) + (operacoes_em_curso > 0 ? 1 : 0);
    printf("Pedido recebido: %s %d (pedidos a frente: %d)\n", tipo == PEDIDO_ARRUMAR ? "arrumar ID" : "dispensar familia", valor, a_frente);

    if (xQueueSend(filaPedidos, &p, 0) != pdTRUE) {
        printf("Fila de pedidos cheia, pedido foi rejeitado\n");
        return false;
    }
    return true;
}

// usado pelo menu para so aceitar comandos manuais nesse caso, NF1 e NF3
bool sistemaLivre() {
    return operacoes_em_curso == 0 && !em_emergencia && (filaPedidos == NULL || uxQueueMessagesWaiting(filaPedidos) == 0);
}


// so para escrever em dd/mm/aaaa hh:mm:ss
static void escreverDataAgora(char* texto, size_t tamanho) {
    time_t t = time(NULL);
    struct tm info;
    localtime_s(&info, &t);
    strftime(texto, tamanho, "%d/%m/%Y %H:%M:%S", &info);
}


static void esperarRetiradaDaCage() {

    if (caixa_entregue_na_cage && caixaNaCage()) {
        printf("Retire da cage a caixa que foi entregue\n");

        while (caixaNaCage()) {
            if (!verificarSegurancaEAtualizarLuzes()) return;   // reset
            vTaskDelay(100);
        }
    }
    caixa_entregue_na_cage = false;
}


static void executarArrumacao(int id_peca){
    
    int familia = ((id_peca - 1) % 3) + 1;   // Familia 1 (1,4,7), familia 2 (2,5,8), familia 3 (3,6,9)

    printf("\nA executar pedido, a arrumar componente %d (familia %d)\n", id_peca, familia);

    // recusar id que ja esteja guardado
    for (int x = 1; x <= 3; x++) {
        for (int z = 1; z <= 3; z++) {
            if (armazem[x][z].ocupado && armazem[x][z].id == id_peca) {
                printf("O componente %d ja esta guardado em (%d,%d), pedido foi rejeitado\n", id_peca, x, z);
                return;   // nao ha movimento
            }
        }
    }

    // procurar a primeira celula vazia
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
        printf("Armazem cheio. Caixa %d nao arrumada\n", id_peca);
        return;   // nao ha movimento
    }

    printf("Celula vazia encontrada, X = %d, Z = %d\n", target_x, target_z);

    iniciarOperacao();
    global_piscar_mode = 1;    // luz de cima pisca durante a arrumacao

    esperarRetiradaDaCage();   

    // o componente entra pela cage no default
    if (!cancelar_operacao)
        pedirMovimentoXZ(1, 1);

    if (!cancelar_operacao && !caixaNaCage()) {
        printf("Coloque a caixa %d na cage\n", id_peca);

        while (!caixaNaCage()) {
            if (!verificarSegurancaEAtualizarLuzes()) break;   // reset
            vTaskDelay(100);
        }
    }

    bool arrumacao_iniciada = false;
    if (!cancelar_operacao) {
        printf("A arrumar a peca\n");
        peca_na_cage = true;   
        id_na_cage = id_peca;
        arrumacao_iniciada = true;
        putPartInCell(target_x, target_z);   // poe peca_na_cage = false quando o mete
    }

    global_piscar_mode = 0;
    resetLuzes();

    if (!arrumacao_iniciada || peca_na_cage) {
        //nao fica registado no armazem no caso de um reset
        printf("Arrumacao do componente %d interrompida.\n", id_peca);
    }
    else {
        //tempo de conclusao
        ULONGLONG tempo_conclusao = GetTickCount64();

        // atualizar o armazem
        armazem[target_x][target_z].id = id_peca;
        armazem[target_x][target_z].familia = familia;
        armazem[target_x][target_z].ocupado = true;
        armazem[target_x][target_z].tempo_storage = tempo_conclusao;
        armazem[target_x][target_z].data_storage = time(NULL);   

        char data[32];
        escreverDataAgora(data, sizeof(data));

        printf("ID da peca: %d\n", id_peca);
        printf("Familia: %d\n", familia);
        printf("Celula selecionada: X=%d, Z=%d\n", target_x, target_z);
        printf("Arrumacao concluida a %s\n\n", data);
    }

    terminarOperacao();
}


static void executarDispensa(int familia_pretendida){

    printf("\nA executar pedido, a dispensar familia %d\n", familia_pretendida);

    // nao tem stock
    bool existe = false;
    for (int x = 1; x <= 3; x++)
        for (int z = 1; z <= 3; z++)
            if (armazem[x][z].ocupado && armazem[x][z].familia == familia_pretendida)
                existe = true;

    if (!existe) {
        printf("Familia %d sem stock. Nao existem pecas disponiveis. Cage nao se moveu\n", familia_pretendida);
        return;
    }

    iniciarOperacao();

    
    //so para garantir que a cage esta vazia
    if (caixaNaCage()) {
        printf("Retire o componente da cage para continuar a dispensa\n");

        while (caixaNaCage()) {
            if (!verificarSegurancaEAtualizarLuzes()) break;   // reset
            vTaskDelay(100);
        }
        if (!cancelar_operacao)
            printf("Cage vazia. A continuar\n");
    }

    caixa_entregue_na_cage = false;

    if (cancelar_operacao) {
        terminarOperacao();
        return;
    }

    //pedido comecou, escolher o mais antigo
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

    printf("Peca mais antiga encontrada em X=%d, Z=%d\n", target_x, target_z);

    int id_peca = armazem[target_x][target_z].id;
    ULONGLONG tempo_armazenamento = armazem[target_x][target_z].tempo_storage;

    global_piscar_mode = 2;    // luz de baixo pisca durante a dispensa 
    peca_na_cage = false;
    id_na_cage = id_peca;

    // retirar a peca
    takePartFromCell(target_x, target_z);

    if (!peca_na_cage) {
        // reset antes de levantar, a caixa vai continuar na celula
        printf("Dispensa interrompida: a caixa %d continua na celula.\n", id_peca);
    }
    else {
        // a caixa saiu de la
        armazem[target_x][target_z].ocupado = false;

        
        pedirMovimentoXZ(1, 1);

        if (cancelar_operacao) {
            printf("Dispensa interrompida: a caixa %d esta na cage\n", id_peca);
        }
        else {
            peca_na_cage = false;                 // entregue
            caixa_entregue_na_cage = true;   //o proximo pedido vai esperar que o tirem

            ULONGLONG duracao = GetTickCount64() - tempo_armazenamento;

            registarDispensa(id_peca, familia_pretendida, tempo_armazenamento);   

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

void vTaskExecutaPedidos(void* pvParameters) {
    Pedido p;

    for (;;) {
        if (xQueueReceive(filaPedidos, &p, portMAX_DELAY) == pdTRUE) {
            if (!estaCalibrado()) {
                printf("\nKit nao calibrado, prime 'c'. Pedido rejeitado\n");
                continue;
            }
            if (p.tipo == PEDIDO_ARRUMAR)
                executarArrumacao(p.valor);
            else
                executarDispensa(p.valor);
        }
    }
}

// agr so conta os cliques do botao de cima, no final coloca um pedido na fila, ja nao arruma diretamente
void vTaskGereBotoes(void* pvParameters) {

    while (1) {
        // Se houve cliques e ja passaram pelo menos 2 segundos desde o ultimo 
        if (contador_impulsos > 0 && (GetTickCount64() - ultimo_tempo_clique) > 2000) {

            int id_peca = contador_impulsos;
            contador_impulsos = 0; // ja foi guardado no id_peca, em cima - reset

            printf("\nContagem terminada. ID da peca: %d\n", id_peca);
            enviarPedido(PEDIDO_ARRUMAR, id_peca);   // rejeita ids fora de 1 a 9
        }

        vTaskDelay(100); // Dá tempo de CPU a outras tarefas
    }
}


// igual mas agr para o botao de baixo
void vTaskGereDispensa(void* pvParameters) {

    while (1) {
        // depois de dois segundos parou de clicar
        if (contador_impulsos_baixo > 0 && (GetTickCount64() - ultimo_tempo_clique_baixo) > 2000) {
            int familia_pretendida = contador_impulsos_baixo;
            contador_impulsos_baixo = 0; // reset

            printf("\ncontagem de dispensa terminada\n");
            printf("Familia solicitada: %d\n", familia_pretendida);
            enviarPedido(PEDIDO_DISPENSAR, familia_pretendida);   // rejeita familias fora de 1 a 3
        }
        vTaskDelay(100);
    }
}


void vTaskLimites(void* pvParameters) {
    
    for (;;) {
        uInt8 p0 = readDigitalU8(0);
        uInt8 p1 = readDigitalU8(1);
        uInt8 p2 = readDigitalU8(2);

        if (getBitValue(p2, 6) && !getBitValue(p0, 2)) // esquerda (P2.6) com X=1 (P0.2 ativo a 0)
            stopX(); 
        if (getBitValue(p2, 7) && !getBitValue(p0, 0)) // direita (P2.7) com X=3 (P0.0)
            stopX(); 

        
        if (getBitValue(p2, 4) && !getBitValue(p0, 5)) // fora (P2.4) com Y=1 (P0.5)
            stopY(); 
        if (getBitValue(p2, 5) && !getBitValue(p0, 3)) // dentro (P2.5) com Y=3 (P0.3)
            stopY(); 

        
        if (getBitValue(p2, 2) && !getBitValue(p1, 3)) // a descer (P2.2) no sensor de baixo do nivel 1 (P1.3)
            stopZ();
        if (getBitValue(p2, 3) && !getBitValue(p0, 6)) // a subir(P2.3) no sensor de cima do nivel 3 (P0.6)
            stopZ();

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
            xSemaphoreGive(sem_X_finished); // diz que o x terminou
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
            xSemaphoreGive(sem_Z_finished); // diz que o z terminou
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
        // espera por um x ou z
        if (xQueueReceive(mbx_XZ, &coords, portMAX_DELAY) == pdTRUE) {

            // envia a coordenada x para a mailbox do x
            xQueueSend(mbx_X, &coords.x, portMAX_DELAY);

            //envia a coordenada z para a mailbox do z
            xQueueSend(mbx_Z, &coords.z, portMAX_DELAY);

            // espera pelos dois
            xSemaphoreTake(sem_X_finished, portMAX_DELAY);
            xSemaphoreTake(sem_Z_finished, portMAX_DELAY);

            // se houver um semaforo de conclusao xz, diz nos
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

//luzes
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

// mais rapida para a emergencia
void piscarDuasLuzesFast() {

    ULONGLONG tempo_atual = GetTickCount64();
    if ((tempo_atual - ultimo_tempo_fast) >= 100) {
        estado_fast = !estado_fast;
        AtivarLuzCima(estado_fast);
        AtivarLuzBaixo(estado_fast);
        ultimo_tempo_fast = tempo_atual;
    }
}
