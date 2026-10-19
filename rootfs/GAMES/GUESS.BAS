10 REM GUESS.BAS - a classic, with line numbers
20 N% = RND(100)
30 T% = 0
40 PRINT "I'm thinking of a number from 1 to 100."
50 INPUT "Your guess? " G%
60 T% = T% + 1
70 IF G% < N% THEN PRINT "Higher!" : GOTO 50
80 IF G% > N% THEN PRINT "Lower!" : GOTO 50
90 PRINT "Got it in "; T%; " guesses."
100 END
