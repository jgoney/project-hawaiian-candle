// Interactive starting point: stop immediately after player-1 tuning is copied.
focus maincpu
bp dd24,1,{ logerror "P1 bike tuning loaded: index=%X record_max_gear=%X top_threshold=%04X\n",w@fff502,w@ff0546,w@ff055e }
g
