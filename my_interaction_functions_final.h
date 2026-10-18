#pragma once
#include <windows.h>
#include <time.h>

extern "C" {
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <semphr.h>
#include <interface.h>
}

// ---------------------------------------------------------------
// ARMAZEM (partilhado com as consultas FR11-FR15)
// ---------------------------------------------------------------
struct CelulaArmazem {
    int id;
    int familia;
    bool ocupado;
    ULONGLONG tempo_storage;   // tick no fim da arrumacao (tempos e ordem FIFO)
    time_t data_storage;       // [Rodrigo] data/hora real da arrumacao (para mostrar)
};

extern CelulaArmazem armazem[4][4];   // [Rodrigo] definido no my_interaction_functions.cpp

// ---------------------------------------------------------------
// TAREFAS GOTO X / Z / XZ (slides do Lab 4)
// ---------------------------------------------------------------
typedef struct {
    int x;
    int z;
} CoordenadasXZ;

typedef struct {
    xQueueHandle mbx_X;
    xSemaphoreHandle sem_X_finished;
} TaskGotoXParams;

typedef struct {
    xQueueHandle mbx_Z;
    xSemaphoreHandle sem_Z_finished;
} TaskGotoZParams;

typedef struct {
    xQueueHandle mbx_XZ;
    TaskGotoXParams* params_X;
    TaskGotoZParams* params_Z;
    xSemaphoreHandle sem_XZ_finished;
} TaskGotoXZParams;

extern xTaskHandle emergencyTask;
extern xTaskHandle resumeTask;
extern xTaskHandle taskSenderCoord;

void setGotoXZHandles(xQueueHandle mbx, xSemaphoreHandle sem);
void pedirMovimentoXZ(int x, int z);

void goto_X_task(void* pvParameters);
void goto_Z_task(void* pvParameters);
void goto_XZ_task(void* pvParameters);

// ---------------------------------------------------------------
// FUNCOES BASE DE BITS
// ---------------------------------------------------------------
int getBitValue(uInt8 value, uInt8 bit_n);
void setBitValue(uInt8* variable, int n_bit, int new_value_bit);

// X axis related functions
void moveXLeft();
void moveXRight();
void stopX();
int  getXPos();
void gotoX(int x);

// Y axis related functions
void moveYInside();
void moveYOutside();
void stopY();
int  getYPos();
void gotoY(int y);

// Z axis related functions
void moveZUp();
void moveZDown();
void stopZ();
int  getZPos();
void gotoZ(int z);

void putPartInCell(int x, int z);
void takePartFromCell(int x, int z);

// calibrar
void calibrarKit();
bool estaCalibrado();

// leitura dos botoes
bool botaoCimaPressionado();
bool botaoBaixoPressionado();
bool caixaNaCage();   // [Rodrigo] sensor P1.4: ha uma caixa dentro da cage

// controlo das luzes
void AtivarLuzCima(bool state);
void AtivarLuzBaixo(bool state);

// ---------------------------------------------------------------
// [NF1] FILA UNICA DE PEDIDOS
// Botoes e teclado so colocam pedidos na fila; a vTaskExecutaPedidos
// executa-os um de cada vez, por ordem de chegada.
// ---------------------------------------------------------------
typedef enum {
    PEDIDO_ARRUMAR,     // valor = ID do componente (1 a 9)   (FR3)
    PEDIDO_DISPENSAR    // valor = familia (1 a 3)             (FR4)
} TipoPedido;

typedef struct {
    TipoPedido tipo;
    int valor;
} Pedido;

void iniciarFilaPedidos();                       // chamar no arranque, antes de criar as tarefas
bool enviarPedido(TipoPedido tipo, int valor);   // valida o valor e coloca na fila
bool sistemaLivre();                             // sem pedidos em curso/em espera e sem emergencia
void vTaskExecutaPedidos(void* pvParameters);

// [NF4] tarefa de seguranca: corta o motor no fim do curso de cada eixo
void vTaskLimites(void* pvParameters);

// interrupts e tarefas dos botoes / emergencia
void switch1_rising_isr(ULONGLONG lastTime);
void switch2_rising_isr(ULONGLONG lastTime);
void vTaskGereBotoes(void* pvParameters);
void vTaskGereDispensa(void* pvParameters);
void vTaskEmergency(void* pvParameters);
void vTaskResume(void* pvParameters);
