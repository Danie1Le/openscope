# The timebase is a display viewport, not an acquisition setting

On a real oscilloscope `TIMebase:SCALe` sets the capture window and the sample rate is derived from it. In OpenScope the sample rate (`ACQ:SRAT`) and record length (`ACQ:POIN`) alone define a capture, and the timebase only zooms and pans the display over it. We chose this to avoid derived, interdependent acquisition settings; the cost is that scripts written for real scopes that rely on `TIM:SCAL` to change what is captured will not behave the same here.
