#include <stdio.h>
#include "my_interaction_functions.h"

extern "C" {
#include <FreeRTOS.h>
#include <task.h>
#include <interface.h>
}

// Criação real do armazém e das estatísticas na memória
Celula armazem[4][4];
EstatisticasArmazem estatisticas;

// =====================================================
// MAPA DE PORTOS / BITS (tudo num só sítio)
// =====================================================
// Fontes: slides 14 e 22 do Lab 3 (vTaskVertical), slide 16 do Lab 2 (getZPos)
// e as verificações de limites comentadas no interface.c.

// Porto 2 - saídas (1 = ligado)
#define BIT_X_ESQUERDA   6
#define BIT_X_DIREITA    7
#define BIT_Z_BAIXO      2
#define BIT_Z_CIMA       3
#define BIT_Y_FORA       4
#define BIT_Y_DENTRO     5
#define BIT_LUZ_CIMA     0   // CONFIRMAR no simulador (bits 0 e 1 são os únicos livres)
#define BIT_LUZ_BAIXO    1   // CONFIRMAR no simulador
#define MASCARA_MOTORES  0xFC   // bits 2 a 7 do porto 2 = motores (bits 0 e 1 = luzes)

// Porto 1 - botões da esquerda (slide 25 do Lab 3: switch1 = P1.5, switch2 = P1.6)
#define BIT_BOTAO_CIMA   5
#define BIT_BOTAO_BAIXO  6

// Sensores de Z (ativos a 0). Índice = nível 1..3; índice 0 não usado.
static const int Z_BAIXO_PORTO[4] = { 0, 1, 1, 0 };
static const int Z_BAIXO_BIT[4] = { 0, 3, 1, 7 };   // P1.3, P1.1, P0.7
static const int Z_CIMA_PORTO[4] = { 0, 1, 1, 0 };
static const int Z_CIMA_BIT[4] = { 0, 2, 0, 6 };   // P1.2, P1.0, P0.6  -> CONFIRMAR P1.2 e P1.0

// Tempo máximo que um movimento pode demorar até chegar ao sensor.
// Se for ultrapassado, o motor pára e aparece um erro (sensor/bit errado ou kit encravado).
// Aumentar se o simulador estiver com uma velocidade muito baixa.
#define TEMPO_MAX_MOVIMENTO_MS  30000

// Estado do porto 1 com os botões em repouso (evita ter de saber a polaridade)
static uInt8 repouso_botoes = 0;

// =====================================================
// ESTADO DE EMERGÊNCIA E DA CAGE
// =====================================================
static volatile int emergencia_ativa = 0;  // STOP em curso
static volatile int cancelar_operacao = 0;  // RESET pedido: os movimentos em curso desistem
static uInt8 motores_guardados = 0;         // motores ligados no momento do STOP (repostos no RESUME)
static volatile int peca_na_cage = 0;       // 1 se há um componente na cage

// -----------------------------------------------------
// FUNÇÕES BASE
// -----------------------------------------------------

int lerBit(uInt8 valor, uInt8 n_bit)
// dado um byte, devolve o valor do seu bit n
{
    return(valor & (1 << n_bit));
}

void escreverBit(uInt8* variavel, int n_bit, int novo_valor)
// dado um byte, coloca o bit n com o valor indicado
{
    uInt8 mascara_ligar = (uInt8)(1 << n_bit);
    uInt8 mascara_desligar = ~mascara_ligar;
    if (novo_valor) *variavel |= mascara_ligar;
    else            *variavel &= mascara_desligar;
}

// Os sensores do kit são ativos a 0
static int sensorAtivo(int porto, int bit)
{
    return !lerBit(readDigitalU8(porto), (uInt8)bit);
}

// Lê o porto 2, altera dois bits e volta a escrever, tudo numa secção crítica.
// O porto 2 é partilhado por motores e luzes: assim nenhuma tarefa estraga
// o que outra escreveu (condição de corrida, slides 17-22 do Lab 3).
//  - Em STOP: as ordens para os motores só alteram a cópia guardada (aplicada
//    no RESUME); os motores reais continuam parados. As luzes funcionam normalmente.
//  - Com RESET pedido: nenhum motor pode arrancar.
static void escreverBitsPorto2(int bit_a, int valor_a, int bit_b, int valor_b)
{
    taskENTER_CRITICAL();
    uInt8 p = readDigitalU8(2);

    if (emergencia_ativa) {
        uInt8 estado = (uInt8)((p & ~MASCARA_MOTORES) | motores_guardados);
        escreverBit(&estado, bit_a, valor_a);
        escreverBit(&estado, bit_b, valor_b);
        motores_guardados = estado & MASCARA_MOTORES;   // ordem fica guardada
        p = (uInt8)(estado & ~MASCARA_MOTORES);          // motores reais parados
    }
    else {
        escreverBit(&p, bit_a, valor_a);
        escreverBit(&p, bit_b, valor_b);
        if (cancelar_operacao)
            p &= (uInt8)~MASCARA_MOTORES;
    }

    writeDigitalU8(2, p);
    taskEXIT_CRITICAL();
}

// Um passo de espera dentro de um ciclo de movimento.
// Devolve 1 para continuar a esperar, 0 para desistir:
//  - RESET pedido                         -> desiste em silêncio
//  - tempo máximo ultrapassado            -> desiste e mostra erro
// O tempo passado em STOP não conta (o relógio recomeça enquanto houver emergência).
static int continuarEspera(ULONGLONG* inicio, const char* quem, int destino)
{
    if (cancelar_operacao) return 0;

    if (emergencia_ativa) {
        *inicio = GetTickCount64();
    }
    else if (GetTickCount64() - *inicio > TEMPO_MAX_MOVIMENTO_MS) {
        printf("\n[%s] ERRO: sensor da posicao %d nao detetado em %d s. "
            "Motor parado. Verifique o mapa de bits.\n",
            quem, destino, TEMPO_MAX_MOVIMENTO_MS / 1000);
        return 0;
    }
    vTaskDelay(1);   // deixa as outras tarefas correr
    return 1;
}

// -----------------------------------------------------
// EMERGÊNCIA (FR5, FR6, FR7)
// -----------------------------------------------------

void ativarEmergencia()
{
    taskENTER_CRITICAL();
    if (!emergencia_ativa) {
        uInt8 p = readDigitalU8(2);
        motores_guardados = p & MASCARA_MOTORES;            // lembra o que estava a andar
        writeDigitalU8(2, (uInt8)(p & ~MASCARA_MOTORES));    // pára todos os motores
        emergencia_ativa = 1;
    }
    taskEXIT_CRITICAL();
}

void retomarDeEmergencia()
{
    taskENTER_CRITICAL();
    if (emergencia_ativa) {
        uInt8 p = readDigitalU8(2);
        writeDigitalU8(2, (uInt8)((p & ~MASCARA_MOTORES) | motores_guardados));
        emergencia_ativa = 0;
    }
    taskEXIT_CRITICAL();
}

void cancelarOperacao()
{
    taskENTER_CRITICAL();
    cancelar_operacao = 1;
    emergencia_ativa = 0;
    motores_guardados = 0;
    uInt8 p = readDigitalU8(2);
    writeDigitalU8(2, (uInt8)(p & ~MASCARA_MOTORES));
    taskEXIT_CRITICAL();
}

void limparCancelamento() { cancelar_operacao = 0; }
int  emEmergencia() { return emergencia_ativa; }
int  operacaoCancelada() { return cancelar_operacao; }

int  pecaNaCage() { return peca_na_cage; }
void definirPecaNaCage(int existe) { peca_na_cage = existe; }

// -----------------------------------------------------
// EIXO X
// -----------------------------------------------------

void moverXEsquerda() { escreverBitsPorto2(BIT_X_ESQUERDA, 1, BIT_X_DIREITA, 0); }
void moverXDireita() { escreverBitsPorto2(BIT_X_ESQUERDA, 0, BIT_X_DIREITA, 1); }
void pararX() { escreverBitsPorto2(BIT_X_ESQUERDA, 0, BIT_X_DIREITA, 0); }

int obterPosX()
{
    uInt8 p = readDigitalU8(0);
    if (!lerBit(p, 2)) return 1;
    if (!lerBit(p, 1)) return 2;
    if (!lerBit(p, 0)) return 3;
    return(-1);
}

void irParaX(int x)
{
    if (cancelar_operacao) return;
    if (x < 1 || x > 3) {                       // NF4
        printf("\n[irParaX] posicao invalida: %d", x);
        return;
    }
    if (obterPosY() != 2) {                     // NF5
        printf("\n[irParaX] recusado: Y nao esta no centro");
        return;
    }
    int pos = obterPosX();
    if (pos == -1) {
        printf("\n[irParaX] posicao X desconhecida (calibrar primeiro)");
        return;
    }
    if (pos == x) return;

    if (pos < x) moverXDireita();
    else         moverXEsquerda();

    // Em STOP os motores estão parados e o sensor não muda: o ciclo simplesmente espera
    ULONGLONG inicio = GetTickCount64();
    while (obterPosX() != x) {
        if (!continuarEspera(&inicio, "irParaX", x)) break;
    }
    pararX();
}

// -----------------------------------------------------
// EIXO Y
// -----------------------------------------------------

void moverYDentro() { escreverBitsPorto2(BIT_Y_FORA, 0, BIT_Y_DENTRO, 1); }
void moverYFora() { escreverBitsPorto2(BIT_Y_FORA, 1, BIT_Y_DENTRO, 0); }
void pararY() { escreverBitsPorto2(BIT_Y_FORA, 0, BIT_Y_DENTRO, 0); }

int obterPosY()
{
    uInt8 p = readDigitalU8(0);
    if (!lerBit(p, 5)) return 1;   // P0.5 fora da cage
    if (!lerBit(p, 4)) return 2;   // P0.4 centro
    if (!lerBit(p, 3)) return 3;   // P0.3 dentro da célula
    return(-1);
}

void irParaY(int y)
{
    if (cancelar_operacao) return;
    if (y < 1 || y > 3) {                       // NF4
        printf("\n[irParaY] posicao invalida: %d", y);
        return;
    }
    int pos = obterPosY();
    if (pos == -1) {
        printf("\n[irParaY] posicao Y desconhecida (calibrar primeiro)");
        return;
    }
    if (pos == y) return;

    if (pos < y) moverYDentro();
    else         moverYFora();

    ULONGLONG inicio = GetTickCount64();
    while (obterPosY() != y) {
        if (!continuarEspera(&inicio, "irParaY", y)) break;
    }
    pararY();
}

// -----------------------------------------------------
// EIXO Z
// -----------------------------------------------------

void moverZCima() { escreverBitsPorto2(BIT_Z_BAIXO, 0, BIT_Z_CIMA, 1); }
void moverZBaixo() { escreverBitsPorto2(BIT_Z_BAIXO, 1, BIT_Z_CIMA, 0); }
void pararZ() { escreverBitsPorto2(BIT_Z_BAIXO, 0, BIT_Z_CIMA, 0); }

// Nível 1..3 pelos sensores "de baixo" (os únicos usados para posicionar)
int obterPosZ()
{
    for (int z = 1; z <= 3; z++)
        if (sensorAtivo(Z_BAIXO_PORTO[z], Z_BAIXO_BIT[z])) return z;
    return(-1);
}

// Nível 1..3 pelos sensores "de cima" (só usados para pôr/tirar peças)
int obterPosZCima()
{
    for (int z = 1; z <= 3; z++)
        if (sensorAtivo(Z_CIMA_PORTO[z], Z_CIMA_BIT[z])) return z;
    return(-1);
}

void irParaZ(int z)
{
    if (cancelar_operacao) return;
    if (z < 1 || z > 3) {                       // NF4
        printf("\n[irParaZ] posicao invalida: %d", z);
        return;
    }
    if (obterPosY() != 2) {                     // NF5
        printf("\n[irParaZ] recusado: Y nao esta no centro");
        return;
    }
    int pos = obterPosZ();
    if (pos == -1) {
        printf("\n[irParaZ] posicao Z desconhecida (calibrar primeiro)");
        return;
    }
    if (pos == z) return;

    if (pos < z) moverZCima();
    else         moverZBaixo();

    ULONGLONG inicio = GetTickCount64();
    while (obterPosZ() != z) {
        if (!continuarEspera(&inicio, "irParaZ", z)) break;
    }
    pararZ();
}

// Sobe do sensor de baixo para o sensor de cima do MESMO nível.
// Pode ser chamada com Y==3 (faz parte de pôr/tirar, não é uma deslocação entre células).
int subirNoNivel()
{
    if (cancelar_operacao) return 0;
    int z = obterPosZ();
    if (z == -1) {
        printf("\n[subirNoNivel] a cage nao esta num sensor de baixo");
        return 0;
    }
    moverZCima();
    ULONGLONG inicio = GetTickCount64();
    while (!sensorAtivo(Z_CIMA_PORTO[z], Z_CIMA_BIT[z])) {
        if (!continuarEspera(&inicio, "subirNoNivel", z)) { pararZ(); return 0; }
    }
    pararZ();
    return 1;
}

// Desce do sensor de cima para o sensor de baixo do MESMO nível.
int descerNoNivel()
{
    if (cancelar_operacao) return 0;
    int z = obterPosZCima();
    if (z == -1) {
        printf("\n[descerNoNivel] a cage nao esta num sensor de cima");
        return 0;
    }
    moverZBaixo();
    ULONGLONG inicio = GetTickCount64();
    while (!sensorAtivo(Z_BAIXO_PORTO[z], Z_BAIXO_BIT[z])) {
        if (!continuarEspera(&inicio, "descerNoNivel", z)) { pararZ(); return 0; }
    }
    pararZ();
    return 1;
}

// -----------------------------------------------------
// MOVIMENTO MANUAL POR PASSOS
// -----------------------------------------------------

void passoX(int dir)
{
    if (cancelar_operacao) return;
    int pos = obterPosX();
    if (pos != -1) {                            // posição conhecida: vai para a vizinha
        int destino = pos + dir;
        if (destino < 1 || destino > 3) {
            printf("\n[passoX] limite atingido");
            return;
        }
        irParaX(destino);
        return;
    }
    // posição desconhecida: anda até encontrar um sensor (os extremos têm sensor)
    if (obterPosY() != 2) {
        printf("\n[passoX] recusado: Y nao esta no centro");
        return;
    }
    if (dir > 0) moverXDireita();
    else         moverXEsquerda();
    ULONGLONG inicio = GetTickCount64();
    while (obterPosX() == -1) {
        if (!continuarEspera(&inicio, "passoX", -1)) break;
    }
    pararX();
}

void passoY(int dir)
{
    if (cancelar_operacao) return;
    int pos = obterPosY();
    if (pos != -1) {
        int destino = pos + dir;
        if (destino < 1 || destino > 3) {
            printf("\n[passoY] limite atingido");
            return;
        }
        irParaY(destino);
        return;
    }
    if (dir > 0) moverYDentro();
    else         moverYFora();
    ULONGLONG inicio = GetTickCount64();
    while (obterPosY() == -1) {
        if (!continuarEspera(&inicio, "passoY", -1)) break;
    }
    pararY();
}

void passoZ(int dir)
{
    if (cancelar_operacao) return;
    int pos = obterPosZ();
    if (pos != -1) {
        int destino = pos + dir;
        if (destino < 1 || destino > 3) {
            printf("\n[passoZ] limite atingido");
            return;
        }
        irParaZ(destino);
        return;
    }
    if (obterPosY() != 2) {
        printf("\n[passoZ] recusado: Y nao esta no centro");
        return;
    }
    if (dir > 0 && obterPosZCima() == 3) {      // já está no limite de cima
        printf("\n[passoZ] limite atingido");
        return;
    }
    if (dir > 0) moverZCima();
    else         moverZBaixo();
    // pára no primeiro sensor de baixo; a subir pára também no limite de cima
    ULONGLONG inicio = GetTickCount64();
    while (obterPosZ() == -1 && !(dir > 0 && obterPosZCima() == 3)) {
        if (!continuarEspera(&inicio, "passoZ", -1)) break;
    }
    pararZ();
}

// -----------------------------------------------------
// CALIBRAÇÃO SEMIAUTOMÁTICA (FR2)
// -----------------------------------------------------

// Espera até a posição do eixo ser conhecida (!= -1).
// Devolve 1 se encontrou sensor, 0 se foi cancelada ou esgotou o tempo.
static int esperarPosicaoConhecida(int (*obterPos)(), const char* quem)
{
    ULONGLONG inicio = GetTickCount64();
    while (obterPos() == -1) {
        if (!continuarEspera(&inicio, quem, -1)) return 0;
    }
    return 1;
}

int calibrar()
{
    if (cancelar_operacao) return 0;
    printf("\n[CALIBRAR] A calibrar o kit...");
    pararX(); pararY(); pararZ();

    // 1. Y primeiro: recolhe para fora até um sensor (é sempre seguro recolher)
    //    e depois vai para o centro. X e Z só se podem mexer com Y==2 (NF5).
    if (obterPosY() == -1) {
        moverYFora();
        int ok = esperarPosicaoConhecida(obterPosY, "calibrar Y");
        pararY();
        if (!ok) { printf("\n[CALIBRAR] FALHOU ou interrompida em Y.\n"); return 0; }
    }
    irParaY(2);

    // 2. Z: desce até um sensor de baixo (o de mais baixo é o limite inferior)
    if (obterPosZ() == -1) {
        moverZBaixo();
        int ok = esperarPosicaoConhecida(obterPosZ, "calibrar Z");
        pararZ();
        if (!ok) { printf("\n[CALIBRAR] FALHOU ou interrompida em Z.\n"); return 0; }
    }

    // 3. X: anda para a esquerda até um sensor (o mais à esquerda é o limite)
    if (obterPosX() == -1) {
        moverXEsquerda();
        int ok = esperarPosicaoConhecida(obterPosX, "calibrar X");
        pararX();
        if (!ok) { printf("\n[CALIBRAR] FALHOU ou interrompida em X.\n"); return 0; }
    }

    // 4. Posição de referência: entrada/saída (1,1)
    irParaX(1);
    irParaZ(1);

    if (obterPosX() == 1 && obterPosY() == 2 && obterPosZ() == 1) {
        printf("\n[CALIBRAR] Concluido: cage em (X=1, Z=1), Y no centro.\n");
        return 1;
    }
    printf("\n[CALIBRAR] FALHOU: a cage nao chegou a referencia.\n");
    return 0;
}

// -----------------------------------------------------
// OPERAÇÕES NA CÉLULA
// -----------------------------------------------------

// Pré-condição: peça na cage, cage parada em (x,z) no sensor de baixo, Y == 2
void porPecaNaCelula()
{
    subirNoNivel();          // levanta a peça acima do nível da prateleira
    irParaY(3);              // entra na célula
    if (descerNoNivel())     // desce: a peça fica pousada na prateleira
        peca_na_cage = 0;    // a partir daqui a peça está na célula
    irParaY(2);              // sai vazio, por baixo da peça
}

// Pré-condição: cage vazia, parada em (x,z) no sensor de baixo, Y == 2
void tirarPecaDaCelula()
{
    irParaY(3);              // entra por baixo da peça
    if (subirNoNivel())      // levanta a peça da prateleira
        peca_na_cage = 1;    // a partir daqui a peça está na cage
    irParaY(2);              // sai com a peça
    descerNoNivel();         // volta ao sensor de baixo (pronto para irParaX/irParaZ)
}

// -----------------------------------------------------
// BOTÕES E LUZES
// -----------------------------------------------------

// Guarda o porto 1 com os botões largados. Um botão está premido quando
// o seu bit é diferente do de repouso -> funciona seja qual for a polaridade.
void lerRepousoBotoes()
{
    repouso_botoes = readDigitalU8(1);
}

int botaoCimaPremido()
{
    return lerBit((uInt8)(readDigitalU8(1) ^ repouso_botoes), BIT_BOTAO_CIMA) != 0;
}

int botaoBaixoPremido()
{
    return lerBit((uInt8)(readDigitalU8(1) ^ repouso_botoes), BIT_BOTAO_BAIXO) != 0;
}

void definirLuzCima(int acesa)
{
    escreverBitsPorto2(BIT_LUZ_CIMA, acesa, BIT_LUZ_CIMA, acesa);
}

void definirLuzBaixo(int acesa)
{
    escreverBitsPorto2(BIT_LUZ_BAIXO, acesa, BIT_LUZ_BAIXO, acesa);
}
