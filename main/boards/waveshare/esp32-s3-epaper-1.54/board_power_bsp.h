#ifndef __BOARD_POWER_BSP_H__
#define __BOARD_POWER_BSP_H__

// Three independently switched rails feed this board: the e-paper panel, the
// audio section, and the battery path. None of them default to on, which is
// why nothing works until this runs.
//
// The polarity is not uniform and it is not a typo: the e-paper and audio
// rails are active LOW, the battery rail is active HIGH. The On/Off methods
// hide that so callers never have to remember it.
class BoardPowerBsp {
private:
    const int epdPowerPin_;
    const int audioPowerPin_;
    const int vbatPowerPin_;

    static void PowerLedTask(void* arg);

public:
    BoardPowerBsp(int epdPowerPin, int audioPowerPin, int vbatPowerPin);
    ~BoardPowerBsp();
    void PowerEpdOn();
    void PowerEpdOff();
    void PowerAudioOn();
    void PowerAudioOff();
    void VbatPowerOn();
    void VbatPowerOff();
};

#endif
