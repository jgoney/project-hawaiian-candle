// Sample player-1 motion calculation once per 60 calls and continue automatically.
focus maincpu
temp0=0
logerror "RR2TRACE,sample,speed,delta,gear,input,throttle,nitro\n"
bp 1a948,a1==ff0a62 && ((temp0++ % 3c)==0),{ logerror "RR2TRACE,%d,%04X,%04X,%X,%02X,%04X,%04X\n",temp0,w@ff0a74,d1,w@ff0a7a,b@ff062c,w@ff056c,w@ff056e ; g }
g
