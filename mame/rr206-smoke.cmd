// Noninteractive workflow check used by the project tests.
focus maincpu
bp 200,1,{ logerror "RR206 reset reached at PC=%08X\n",pc ; g }
g
