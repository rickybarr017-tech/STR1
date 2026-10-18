#include <iostream>
#include <conio.h>
#include <stdlib.h>
#include <windows.h>

extern "C" {
#include <FreeRTOS.h>
#include <task.h>
#include <timers.h>
#include <semphr.h>
#include <interface.h>
#include <interrupts.h>
}

#include "my_interaction_functions.h"   // [Rodrigo] caminho corrigido (o antigo so funcionava num computador)
#include "consultas.h"                  // [Rodrigo] consultas FR11-FR15

/* =====================================================================
   ALTERACOES NESTE FICHEIRO (procurar pelas etiquetas, Ctrl+F)
   [Rodrigo]  teclas l, f, v, k, n (consultas FR11-FR15)
   [NF1]      teclas g e t: pedidos de arrumar/dispensar pelo teclado (vao para a
              mesma fila que os botoes); comandos manuais so com o sistema parado;
              criacao da fila e da tarefa vTaskExecutaPedidos
   [NF4]      criacao da tarefa vTaskLimites
   [NF5]      teclas de movimento manual em X e Z so com o Y no centro
   ===================================================================== */

#define mainREGION_1_SIZE   8201
#define mainREGION_2_SIZE   29905
#define mainREGION_3_SIZE   7607

void vAssertCalled(unsigned long ulLine, const char* const pcFileName)
{
    static BaseType_t xPrinted = pdFALSE;
    volatile uint32_t ulSetToNonZeroInDebuggerToContinue = 0;
    (void)ulLine;
    (void)pcFileName;
    printf("ASSERT! Line %ld, file %s, GetLastError() %ld\r\n", ulLine, pcFileName, GetLastError());

    taskENTER_CRITICAL();
    {
        __debugbreak();
        while (ulSetToNonZeroInDebuggerToContinue == 0)
        {
            __asm { NOP };
            __asm { NOP };
        }
    }
    taskEXIT_CRITICAL();
}


static void initialiseHeap(void)
{
    static uint8_t ucHeap[configTOTAL_HEAP_SIZE];
    volatile uint32_t ulAdditionalOffset = 19;
    const HeapRegion_t xHeapRegions[] =
    {
        { ucHeap + 1, mainREGION_1_SIZE },
        { ucHeap + 15 + mainREGION_1_SIZE, mainREGION_2_SIZE },
        { ucHeap + 19 + mainREGION_1_SIZE + mainREGION_2_SIZE, mainREGION_3_SIZE },
        { NULL, 0 }
    };

    configASSERT((ulAdditionalOffset + mainREGION_1_SIZE + mainREGION_2_SIZE + mainREGION_3_SIZE) < configTOTAL_HEAP_SIZE);
    (void)ulAdditionalOffset;
    vPortDefineHeapRegions(xHeapRegions);
}


//hooks
void myTickHook(void) {
    Sleep(0);
}

void myIdleHook(void) {
    Sleep(0);
}

// [NF1] Comandos manuais (movimentos, gotos, calibrar, 'h') so com o sistema parado:
// sem pedidos em curso/em espera e sem emergencia. Assim nao interferem com a operacao.
static bool manualPermitido() {
    if (!sistemaLivre()) {
        printf("[NF1] Comando manual recusado: ha pedidos em curso ou o sistema esta em STOP.\n");
        return false;
    }
    return true;
}

// [NF5] Movimentos manuais em X e Z so com o Y parado no centro
static bool yNoCentro() {
    if (getYPos() != 2) {
        printf("[NF5] Movimento recusado: o Y tem de estar no centro (tecla e).\n");
        return false;
    }
    return true;
}

// menu
void vTaskMenu(void* pvParameters)
{
    int tecla = 0;
    bool luzCimaEstado = false;
    bool luzBaixoEstado = false;

    printf("        MENU          \n");
    printf("   'a' / 'd' : Mover Eixo X (Esquerda / Direita)\n");
    printf("   'w' / 's' : Mover Eixo Z (Cima / Baixo)\n");
    printf("   'u' / 'i' : Mover Eixo Y (Dentro / Fora)\n");
    printf("   'x'       : Parar todos os motores\n");
    printf("   '1'-'3'   : Goto X (1 a 3)\n");
    printf("   '4'-'6'   : Goto Z (1 a 3)\n");
    printf("   'q','e','r': Goto Y (1 a 3)\n");
    printf("   'c'       : Calibrar o Kit \n");
    printf("   'o' / 'p' : Alternar Luz superior / Luz inferior \n");
    printf("   'b'       : Ler estado dos botoes esquerdos \n"); //para debug, so ver se funcionam
    printf("   'h'       : Guardar (1) ou Retirar (2) caixa (so para testes)\n");
    printf("   'g'       : Pedido: guardar componente (ID 1-9)\n");                  // [NF1]
    printf("   't'       : Pedido: dispensar familia (1-3)\n");                      // [NF1]
    printf("   'l'       : Listar componentes guardados (FR11)\n");             // [Rodrigo]
    printf("   'f'       : Stock e proximo de uma familia (FR12)\n");           // [Rodrigo]
    printf("   'v'       : Ver o conteudo de uma celula (FR13)\n");             // [Rodrigo]
    printf("   'k'       : Historico de dispensas (FR14)\n");                   // [Rodrigo]
    printf("   'n'       : Numero de dispensas por familia (FR15)\n");          // [Rodrigo]
    printf("   Botoes: cliques em cima = guardar ID, em baixo = dispensar familia\n");   // [FR5-FR10]
    printf("           os dois juntos = STOP; em STOP: cima = RESUME, baixo = RESET\n"); // [FR5-FR10]
    printf("   Os comandos manuais so funcionam com o sistema parado.\n");               // [NF1]


    while (1) {
        if (_kbhit()) { // faz uma verificação simples no teclado, ve se ha alguma tecla à espera de ser lida, sem parar o programa, true se houver e lê com getch, para nao bloquear o programa
            tecla = _getch();

            if (tecla == 'd') {
                if (manualPermitido() && yNoCentro()) {     // [NF1][NF5]
                    printf("vai mover para a direita\n");
                    moveXRight();                             // [NF4] a vTaskLimites para no fim do curso
                }
            }

            else if (tecla == 'a') {
                if (manualPermitido() && yNoCentro()) {     // [NF1][NF5]
                    printf("vai mover para esquerda\n");
                    moveXLeft();
                }
            }

            else if (tecla == 'x') {
                stopX();
                stopY();
                stopZ();
                printf("vai parar\n");
            }

            else if (tecla == 'w') {
                if (manualPermitido() && yNoCentro()) {     // [NF1][NF5]
                    printf("vai mover para cima\n");
                    moveZUp();
                }
            }

            else if (tecla == 's') {
                if (manualPermitido() && yNoCentro()) {     // [NF1][NF5]
                    printf("vai mover para baixo\n");
                    moveZDown();
                }
            }

            else if (tecla == 'u') {
                if (manualPermitido()) {                    // [NF1]
                    printf("vai para dentro\n");
                    moveYInside();
                }
            }

            else if (tecla == 'i') {
                if (manualPermitido()) {                    // [NF1]
                    printf("vai para fora\n");
                    moveYOutside();
                }
            }

            // calibracao
            else if (tecla == 'c' || tecla == 'C') {
                if (manualPermitido())                      // [NF1]
                    calibrarKit();
            }

            // Leitura dos Botões
            else if (tecla == 'b' || tecla == 'B') {
                printf("Botao Superior: ");
                if (botaoCimaPressionado()) {
                    printf("premido\n");
                }
                else {
                    printf("solto\n");
                }

                printf("Botao Inferior: ");
                if (botaoBaixoPressionado()) {
                    printf("premido\n");
                }
                else {
                    printf("solto\n");
                }
            }

            // teste de luzes
            else if (tecla == 'o' || tecla == 'O') {
                luzCimaEstado = !luzCimaEstado; //mudar de true para false ou contrario
                AtivarLuzCima(luzCimaEstado);

                if (luzCimaEstado) {
                    printf("Luz de cima ligada\n");
                }
                else {
                    printf("Luz de cima desligada\n");
                }
            }
            else if (tecla == 'p' || tecla == 'P') {
                luzBaixoEstado = !luzBaixoEstado;
                AtivarLuzBaixo(luzBaixoEstado);

                if (luzBaixoEstado) {
                    printf("Luz de baixo ligada\n");
                }
                else {
                    printf("Luz de baixo desligada\n");
                }
            }

            // Goto's  ([NF1] so com o sistema parado; [NF5] o gotoX/gotoZ exigem Y no centro)
            else if (tecla >= '1' && tecla <= '6') {
                if (!estaCalibrado()) {
                    printf("O kit ainda nao foi calibrado! Prime 'c' para calibrar primeiro.\n");
                }
                else if (manualPermitido()) {
                    if (tecla <= '3') {
                        printf("vai para X%c\n", tecla);
                        gotoX(tecla - '0');
                    }
                    else {
                        printf("vai para Z%c\n", tecla - 3);
                        gotoZ(tecla - '3');
                    }
                }
            }

            else if (tecla == 'q' || tecla == 'e' || tecla == 'r') {
                if (!estaCalibrado()) {
                    printf("O kit ainda nao foi calibrado! Prime 'c' para calibrar primeiro.\n");
                }
                else if (manualPermitido()) {
                    int y = (tecla == 'q') ? 1 : (tecla == 'e') ? 2 : 3;
                    printf("vai para Y%d\n", y);
                    gotoY(y);
                }
            }

            // [NF1] pedido de arrumacao pelo teclado (vai para a mesma fila que os botoes)
            else if (tecla == 'g' || tecla == 'G') {
                int id = 0;
                printf("\nID do componente a guardar (1 a 9): ");
                std::cin >> id;
                enviarPedido(PEDIDO_ARRUMAR, id);   // [NF3] rejeita fora de 1 a 9
            }

            // [NF1] pedido de dispensa pelo teclado
            else if (tecla == 't' || tecla == 'T') {
                int familia = 0;
                printf("\nFamilia a dispensar (1 a 3): ");
                std::cin >> familia;
                enviarPedido(PEDIDO_DISPENSAR, familia);   // [NF3] rejeita fora de 1 a 3
            }

            // [Rodrigo] FR11 - lista de componentes guardados
            else if (tecla == 'l' || tecla == 'L') {
                mostrarComponentesGuardados();
            }

            // [Rodrigo] FR12 - stock e proximo componente de uma familia
            else if (tecla == 'f' || tecla == 'F') {
                int familia = 0;
                printf("\nFamilia (1 a 3): ");
                std::cin >> familia;
                if (familia >= 1 && familia <= 3)
                    mostrarFamilia(familia);
                else
                    printf("Familia invalida\n");
            }

            // [Rodrigo] FR13 - ver o conteudo de uma celula
            else if (tecla == 'v' || tecla == 'V') {
                int x = 0, z = 0;
                printf("\nColuna X (1 a 3): ");
                std::cin >> x;
                printf("Linha Z (1 a 3): ");
                std::cin >> z;
                if (x >= 1 && x <= 3 && z >= 1 && z <= 3)
                    mostrarCelula(x, z);
                else
                    printf("Celula invalida\n");
            }

            // [Rodrigo] FR14 - historico de dispensas
            else if (tecla == 'k' || tecla == 'K') {
                mostrarHistorico();
            }

            // [Rodrigo] FR15 - numero de dispensas por familia
            else if (tecla == 'n' || tecla == 'N') {
                mostrarContagens();
            }

            // opcao h  (so para testes: nao atualiza o inventario)
            else if (tecla == 'h' || tecla == 'H') {
                if (!estaCalibrado()) {
                    printf("O kit precisa de ser calibrado primeiro, prime 'c'\n");
                }
                else if (manualPermitido()) {               // [NF1]
                    int opcao = 0, x = 0, z = 0;
                    printf("\nGuardar caixa (1) ou tirar (2)? ");
                    std::cin >> opcao;

                    if (opcao == 1 || opcao == 2) {
                        printf("Introduza a coluna X (1 a 3): ");
                        std::cin >> x;
                        printf("Introduza a linha Z (1 a 3): ");
                        std::cin >> z;

                        if (x >= 1 && x <= 3 && z >= 1 && z <= 3) {
                            if (opcao == 1)
                                putPartInCell(x, z);
                            else
                                takePartFromCell(x, z);
                        }
                        else {
                            printf("Posicao invalida\n");
                        }
                    }
                    else {
                        printf("Opcao invalida\n");
                    }
                }
            }
        }
        vTaskDelay(50); // liberta a CPU durante 50ms para permitir que outras tarefas corram
    }
}

// inicializar portos
void myDaemonTaskStartupHook(void) {
    printf("\nA inicializar portos de hardware...");
    createDigitalInput(0);
    createDigitalInput(1);
    createDigitalOutput(2);
    writeDigitalU8(2, 0);

    // [NF1] fila unica de pedidos (tem de existir antes das tarefas que a usam)
    iniciarFilaPedidos();

    // Cria a tarefa do menu
    xTaskCreate(vTaskMenu, "Menu", 512, NULL, 1, NULL);

    // [NF1] estas duas tarefas agora so contam cliques e colocam pedidos na fila
    xTaskCreate(vTaskGereBotoes, "Gere Botoes", 512, NULL, 2, NULL);

    // interrupt dos cliques dos botoes esquerdo
    attachInterrupt(1, 5, switch1_rising_isr, RISING);


    xTaskCreate(vTaskGereDispensa, "Gere Dispensa (FR4)", 512, NULL, 2, NULL);

    //interrupt dos cliques dos botoes direito
    attachInterrupt(1, 6, switch2_rising_isr, RISING);

    // [NF1] unica tarefa que executa arrumacoes e dispensas, por ordem de chegada
    xTaskCreate(vTaskExecutaPedidos, "Executa Pedidos (NF1)", 512, NULL, 2, NULL);

    // [NF4] tarefa de seguranca dos limites dos eixos
    xTaskCreate(vTaskLimites, "Limites (NF4)", 256, NULL, 2, NULL);

    // eixo X
    xQueueHandle mbx_X_local = xQueueCreate(10, sizeof(int));
    xSemaphoreHandle sem_X_local = xSemaphoreCreateCounting(10, 0);
    TaskGotoXParams* params_x = (TaskGotoXParams*)pvPortMalloc(sizeof(TaskGotoXParams));
    params_x->mbx_X = mbx_X_local;
    params_x->sem_X_finished = sem_X_local;
    xTaskCreate(goto_X_task, "GOTO X Task", 100, params_x, 1, NULL);

    // eixo Z
    xQueueHandle mbx_Z_local = xQueueCreate(10, sizeof(int));
    xSemaphoreHandle sem_Z_local = xSemaphoreCreateCounting(10, 0);
    TaskGotoZParams* params_z = (TaskGotoZParams*)pvPortMalloc(sizeof(TaskGotoZParams));
    params_z->mbx_Z = mbx_Z_local;
    params_z->sem_Z_finished = sem_Z_local;
    xTaskCreate(goto_Z_task, "GOTO Z Task", 100, params_z, 1, NULL);

    // coordenador XZ
    xQueueHandle mbx_XZ_local = xQueueCreate(10, sizeof(CoordenadasXZ));
    xSemaphoreHandle sem_XZ_local = xSemaphoreCreateCounting(10, 0);
    TaskGotoXZParams* params_xz = (TaskGotoXZParams*)pvPortMalloc(sizeof(TaskGotoXZParams));
    params_xz->mbx_XZ = mbx_XZ_local;
    params_xz->params_X = params_x;
    params_xz->params_Z = params_z;
    params_xz->sem_XZ_finished = sem_XZ_local;
    xTaskCreate(goto_XZ_task, "GOTO XZ Task", 100, params_xz, 1, &taskSenderCoord);

    setGotoXZHandles(mbx_XZ_local, sem_XZ_local);

    //tarefas de emergencia
    xTaskCreate(vTaskEmergency, "vTaskEmergency", 100, NULL, 3, &emergencyTask);
    xTaskCreate(vTaskResume, "vTaskResume", 100, NULL, 3, &resumeTask);
}


int main()
{
    initialiseHeap();
    vApplicationDaemonTaskStartupHook = &myDaemonTaskStartupHook;
    vApplicationTickHook = &myTickHook;
    vApplicationIdleHook = &myIdleHook;

    vTaskStartScheduler();
    return 0;
}
