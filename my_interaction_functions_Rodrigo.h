#pragma once
#include <windows.h>

extern "C" {
#include <interface.h>
   // [Rodrigo] estrutura do armazem passou do .cpp para aqui,
   // para as consultas (FR11-FR15) a poderem ler
    struct CelulaArmazem {
        int id;
        int familia;
        bool ocupado;
        ULONGLONG tempo_storage;   // tick no fim da arrumacao (calcular tempos e ordem FIFO)
        time_t data_storage;       // [Rodrigo] data/hora real da arrumacao (para mostrar)
    };

    extern CelulaArmazem armazem[4][4];   // [Rodrigo] definido no my_interaction_functions.cpp

}

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

//calibrar
void calibrarKit();
bool estaCalibrado();


//leitura dos Botões 
bool botaoCimaPressionado();
bool botaoBaixoPressionado();

//controlo das Luzes
void AtivarLuzCima(bool state);
void AtivarLuzBaixo(bool state);

//com interrupts
void switch1_rising_isr(ULONGLONG lastTime);
void vTaskGereBotoes(void* pvParameters);