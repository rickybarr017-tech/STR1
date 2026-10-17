// [Rodrigo] consultas.cpp - Consultas de informacao do armazem (FR11 a FR15)
#include <stdio.h>
#include <time.h>
#include "my_interaction_functions.h"
#include "consultas.h"

// Converte uma data (time_t) em texto "dd/mm/aaaa hh:mm:ss"
static void formatarData(time_t t, char* texto, size_t tamanho)
{
    struct tm info;
    localtime_s(&info, &t);
    strftime(texto, tamanho, "%d/%m/%Y %H:%M:%S", &info);
}

// Converte um tempo em milissegundos em texto "hh:mm:ss"
static void formatarDuracao(ULONGLONG ms, char* texto, size_t tamanho)
{
    unsigned long s = (unsigned long)(ms / 1000);
    snprintf(texto, tamanho, "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
}

// FR11: lista dos componentes guardados
void mostrarComponentesGuardados()
{
    char data[32], tempo[16];
    int quantos = 0;
    ULONGLONG agora = GetTickCount64();

    printf("\n--- COMPONENTES GUARDADOS (FR11) ---\n");
    printf(" ID  Fam  Celula  Arrumado a            Tempo guardado\n");

    for (int x = 1; x <= 3; x++) {
        for (int z = 1; z <= 3; z++) {
            CelulaArmazem* c = &armazem[x][z];
            if (!c->ocupado) continue;          // celula vazia: passa a seguinte

            formatarData(c->data_storage, data, sizeof(data));
            formatarDuracao(agora - c->tempo_storage, tempo, sizeof(tempo));
            printf(" %2d   %d   (%d,%d)   %s   %s\n", c->id, c->familia, x, z, data, tempo);
            quantos++;
        }
    }

    if (quantos == 0) printf(" (armazem vazio)\n");
}


// FR12: quantos componentes ha de uma familia e qual e o proximo a sair (o mais antigo) (FIFO)
void mostrarFamilia(int familia)
{
    int quantos = 0;
    int px = -1, pz = -1;   // celula do componente mais antigo (ainda nenhuma)

    for (int x = 1; x <= 3; x++) {
        for (int z = 1; z <= 3; z++) {
            CelulaArmazem* c = &armazem[x][z];
            if (!c->ocupado || c->familia != familia) continue;   // vazia ou outra familia

            quantos++;
            // FIFO: o mais antigo e o que tem o menor tempo de arrumacao
            if (px == -1 || c->tempo_storage < armazem[px][pz].tempo_storage) {
                px = x;
                pz = z;
            }
        }
    }

    printf("\n--- FAMILIA %d (FR12) ---\n", familia);
    if (quantos == 0) {
        printf(" Sem stock.\n");
        return;
    }

    char data[32];
    formatarData(armazem[px][pz].data_storage, data, sizeof(data));
    printf(" Disponiveis: %d\n", quantos);
    printf(" Proximo a dispensar: ID %d, celula (%d,%d), arrumado a %s\n",
        armazem[px][pz].id, px, pz, data);
}

// FR13: o que esta numa celula (X,Z), ou "vazia"
void mostrarCelula(int x, int z)
{
    CelulaArmazem* c = &armazem[x][z];

    printf("\n--- CELULA (%d,%d) (FR13) ---\n", x, z);
    if (!c->ocupado) {
        printf(" Vazia.\n");
        return;
    }

    char data[32], tempo[16];
    formatarData(c->data_storage, data, sizeof(data));
    formatarDuracao(GetTickCount64() - c->tempo_storage, tempo, sizeof(tempo));
    printf(" ID %d, familia %d, arrumado a %s, tempo guardado %s\n",
        c->id, c->familia, data, tempo);
}