REM MANDEL.BAS - the Mandelbrot set in text, using real numbers
chars$ = " .:-=+*#%@"
FOR y = -1.2 TO 1.2 STEP 0.1
  line$ = ""
  FOR x = -2.1 TO 0.7 STEP 0.04
    zr = 0 : zi = 0 : n% = 0
    REPEAT
      t = zr*zr - zi*zi + x
      zi = 2*zr*zi + y
      zr = t
      n% += 1
    UNTIL zr*zr + zi*zi > 4 OR n% = 40
    line$ += MID$(chars$, (n% MOD 10) + 1, 1)
  NEXT
  PRINT line$
NEXT
