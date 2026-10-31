- 09_openworld lighting (M-OPENWORLD-LIGHT): the city now uses the atmosphere sky with clouds, SSAO, contact shadows, SSR, a GI volume and a colour-grade LUT;
  its DayCycle carries a stronger daytime ambient so shaded grass is no longer black. `Kit3DExtract`: an enabled DayCycle keeps the ambient when the
  atmosphere is on, and the atmosphere yields to the moon while the moon holds the light (no daylight sky at night). Scenes without both are unchanged.
