// Log every final player-1 motion delta during deterministic input playback.
focus maincpu
temp0=0
logerror "RR2PHYS,sample,speed,delta,gear,flags,slope,input,throttle,nitro\n"
bp 1a948,a1==ff0a62,{ logerror "RR2PHYS,%d,%04X,%04X,%X,%02X,%04X,%02X,%04X,%04X\n",temp0++,w@ff0a74,d1,w@ff0a7a,b@ff0aca,w@ff0aa0,b@ff062c,w@ff056c,w@ff056e ; g }
g
