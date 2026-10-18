#pragma once
// [Rodrigo] consultas.h - Consultas de informacao do armazem (FR11 a FR15)
#include <windows.h>   // ULONGLONG

void mostrarComponentesGuardados();   // FR11
void mostrarFamilia(int familia);     // FR12
void mostrarCelula(int x, int z);     // FR13
void mostrarHistorico();              // FR14
void mostrarContagens();              // FR15

// Chamada pelo FR4 (vTaskGereDispensa) quando o componente e entregue em (1,1)
void registarDispensa(int id, int familia, ULONGLONG tempo_storage);
