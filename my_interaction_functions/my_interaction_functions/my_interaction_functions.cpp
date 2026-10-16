#include "my_interaction_functions.h"

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


// X axis related functions

void moveXLeft()
{
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 6, 1); // set bit 6 to high level
    setBitValue(&p, 7, 0); // set bit 7 to low level
    writeDigitalU8(2, p); // update port 2
}

void moveXRight()
{
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 6, 0); // set bit 6 to low level
    setBitValue(&p, 7, 1); // set bit 7 to high level
    writeDigitalU8(2, p); // update port 2
}

void stopX()
{
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 6, 0); // set bit 6 to low level
    setBitValue(&p, 7, 0); // set bit 7 to low level
    writeDigitalU8(2, p); // update port 2
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

    if (x_pos == -1)
        return;

    if (x_pos < x) {
        moveXRight();

        while (getXPos() != x) {
        }

        stopX();
    }
    else if (x_pos > x) {
        moveXLeft();

        while (getXPos() != x) {
        }

        stopX();
    }
}

// Y axis related functions
void moveYInside() 
{
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 4, 0); // set bit 4 to low level
    setBitValue(&p, 5, 1); // set bit 5 to high level
    writeDigitalU8(2, p); // update port 2
}

void moveYOutside()
{
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 4, 1); // set bit 4 to low level
    setBitValue(&p, 5, 0); // set bit 5 to high level
    writeDigitalU8(2, p); // update port 2
}

void stopY() {
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 4, 0); // set bit 4 to low level
    setBitValue(&p, 5, 0); // set bit 5 to low level
    writeDigitalU8(2, p); // update port 2
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

}

// Z axis related functions

void moveZUp()
{
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 2, 0); // set bit 4 to low level
    setBitValue(&p, 3, 1); // set bit 5 to high level
    writeDigitalU8(2, p); // update port 2
}

void moveZDown()
{
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 2, 1); // set bit 4 to low level
    setBitValue(&p, 3, 0); // set bit 5 to high level
    writeDigitalU8(2, p); // update port 2
}

void stopZ() {
    uInt8 p = readDigitalU8(2); // read port 2
    setBitValue(&p, 2, 0); // set bit 4 to low level
    setBitValue(&p, 3, 0); // set bit 5 to low level
    writeDigitalU8(2, p); // update port 2
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

}