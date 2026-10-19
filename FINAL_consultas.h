#pragma once

#include <windows.h>  

void mostrarComponentesGuardados();   // FR11
void mostrarFamilia(int familia);     // FR12
void mostrarCelula(int x, int z);     // FR13
void mostrarHistorico();              // FR14
void mostrarContagens();              // FR15

// chamada pelo (vTaskGereDispensa) quando o componente e entregue no default
void registarDispensa(int id, int familia, ULONGLONG tempo_storage);
