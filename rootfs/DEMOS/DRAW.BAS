REM DRAW.BAS - a window to draw in, from BASIC (run it on the desktop)
SYS "Win_Open", "Drawing from BASIC", -1, -1, 400, 300 TO w%
SYS "Win_Clear", w%, &FFFFFF
FOR i% = 0 TO 15
  c% = (i% * 16) * &10000 + (255 - i% * 16) * &100 + 128
  SYS "Win_Rect", w%, 20 + i% * 10, 20 + i% * 8, 120, 80, c%
NEXT
FOR a = 0 TO 2 * PI STEP PI / 24
  SYS "Win_Line", w%, 300, 200, 300 + 80 * COS(a), 200 + 80 * SIN(a), &1D3A8A
NEXT
SYS "Win_Text", w%, 20, 270, "Click to paint, close the window to stop", &000000
REPEAT
  SYS "Win_Event", w% TO e%
  IF e% = 2 OR e% = 3 THEN
    SYS "Win_X", w% TO x% : SYS "Win_Y", w% TO y%
    IF e% = 2 THEN SYS "Win_Rect", w%, x% - 3, y% - 3, 6, 6, &D03030
  ENDIF
  IF e% = 0 THEN SYS "OS_Sleep", 10
UNTIL e% = -1
SYS "Win_Close", w%
