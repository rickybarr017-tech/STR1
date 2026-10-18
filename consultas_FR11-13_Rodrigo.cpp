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

// FR12: quantos componentes ha de uma familia e qual e o proximo a sair (o mais antigo)
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

// ---------------------------------------------------------------
// HISTORICO DE DISPENSAS (FR14) E CONTAGENS (FR15)
// ---------------------------------------------------------------

// Um registo por cada dispensa concluida
struct RegistoDispensa {
    int id;
    int familia;
    time_t data_dispensa;     // data/hora real da entrega
    ULONGLONG duracao_ms;     // quanto tempo esteve guardado
};

#define MAX_HISTORICO 100
static RegistoDispensa historico[MAX_HISTORICO];
static int num_historico = 0;
static int dispensas_familia[4] = { 0 };   // indices 1, 2, 3 = familias 1, 2, 3

// Chamada pelo FR4 (vTaskGereDispensa) quando o componente e entregue em (1,1)
void registarDispensa(int id, int familia, ULONGLONG tempo_storage)
{
    if (num_historico < MAX_HISTORICO) {
        historico[num_historico].id            = id;
        historico[num_historico].familia       = familia;
        historico[num_historico].data_dispensa = time(NULL);
        historico[num_historico].duracao_ms    = GetTickCount64() - tempo_storage;
        num_historico++;
    }
    if (familia >= 1 && familia <= 3)
        dispensas_familia[familia]++;
}

// FR14: historico de todas as dispensas concluidas
void mostrarHistorico()
{
    char data[32], tempo[16];

    printf("\n--- HISTORICO DE DISPENSAS (FR14) ---\n");
    if (num_historico == 0) {
        printf(" (nenhuma dispensa concluida)\n");
        return;
    }
    for (int i = 0; i < num_historico; i++) {
        formatarData(historico[i].data_dispensa, data, sizeof(data));
        formatarDuracao(historico[i].duracao_ms, tempo, sizeof(tempo));
        printf(" %2d) ID %d  Familia %d  Dispensado a %s  Tempo guardado %s\n",
               i + 1, historico[i].id, historico[i].familia, data, tempo);
    }
}

// FR15: numero de dispensas por familia e total
void mostrarContagens()
{
    int total = dispensas_familia[1] + dispensas_familia[2] + dispensas_familia[3];

    printf("\n--- DISPENSAS CONCLUIDAS (FR15) ---\n");
    printf(" Familia 1: %d\n", dispensas_familia[1]);
    printf(" Familia 2: %d\n", dispensas_familia[2]);
    printf(" Familia 3: %d\n", dispensas_familia[3]);
    printf(" Total:     %d\n", total);
}
