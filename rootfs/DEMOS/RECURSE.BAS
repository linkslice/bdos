REM RECURSE.BAS - functions, procedures and local variables
PRINT "10! = "; FNfact(10)
PRINT "fib(20) = "; FNfib(20)
PROChanoi(3, "A", "C", "B")
PRINT "sqrt(2) = "; SQR(2); "  pi = "; PI; "  sin(30 deg) = "; SIN(RAD(30))
PRINT "Upper: "; UPPER$("bdos"); "  2^10 = "; 2^10; "  17 DIV 5 = "; 17 DIV 5; "  17 MOD 5 = "; 17 MOD 5
END

DEF FNfact(n)
IF n <= 1 THEN = 1
= n * FNfact(n - 1)

DEF FNfib(n%)
LOCAL a, b, i%
a = 0 : b = 1
FOR i% = 1 TO n%
  SWAP a, b
  b = a + b
NEXT
= a

DEF PROChanoi(n%, from$, to$, via$)
IF n% = 0 THEN ENDPROC
PROChanoi(n% - 1, from$, via$, to$)
PRINT "Move disc "; n%; " from "; from$; " to "; to$
PROChanoi(n% - 1, via$, to$, from$)
ENDPROC
