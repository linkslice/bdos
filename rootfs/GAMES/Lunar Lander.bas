REM Lunar Lander - burn fuel each turn to land gently
CLS
PRINT "LUNAR LANDER"
PRINT "You are 1000 m above the Moon, falling at 50 m/s,"
PRINT "with 150 units of fuel. Each turn lasts 5 seconds."
PRINT "Burn 0-30 units per turn. Touch down below 5 m/s to live."
PRINT
alt = 1000 : vel = 50 : fuel = 150 : turn% = 0
REPEAT
  turn% += 1
  PRINT "Turn "; turn%; ": height "; INT(alt); " m, speed "; INT(vel * 10) / 10; " m/s, fuel "; fuel
  burn = 0
  IF fuel > 0 THEN INPUT "  Burn? " burn
  IF burn < 0 THEN burn = 0
  IF burn > 30 THEN burn = 30
  IF burn > fuel THEN burn = fuel
  fuel -= burn
  newvel = vel + 1.62 * 5 - burn * 0.6
  alt -= (vel + newvel) / 2 * 5
  vel = newvel
UNTIL alt <= 0 OR alt > 2000
IF alt > 2000 THEN PRINT "You flew off into space." : END
PRINT "Touchdown at "; INT(vel * 10) / 10; " m/s."
IF vel < 5 THEN PRINT "A perfect landing!" ELSE IF vel < 15 THEN PRINT "Rough, but you made it." ELSE PRINT "Crater. Sorry."
