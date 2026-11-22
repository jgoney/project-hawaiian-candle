// Log every game-loop timestep handoff during deterministic input playback.
// temp0 counts VBlank interrupts; temp1 counts accumulator increments at 0xE096.
focus maincpu
temp0=0
temp1=0
temp2=0
logerror "RR2TICK,loop,vblanks,increments,timestep,paused\n"
bp 11b8,1,{ temp0++ ; g }
bp e096,1,{ temp1++ ; g }
bp df48,1,{ logerror "RR2TICK,%d,%d,%d,%d,%X\n",temp2++,temp0,temp1,d1,w@ff254e ; g }
g
