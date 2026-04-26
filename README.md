HARDWARE USED:
PRNG_FULL_External_MCP3202.zip - CMOD A7 35T, MCP3202 ADC Board
PRNG_Internal_Fixed_Seed.zip - CMOD A7 35T
led_shooter_game.ino - ESP32S3 WVROOM, Mechanical Keyboard Switch, GF1002 amplifier, 4ohm/0.5W speaker, TFT display (GC9A01A round 240x240), EC56 Rotary Encoder, WS2812B LED Strip (33 LED in Parallel)

Circuit Wiring Diagram in IEEE report.

CMOD A7 35T upload instructions:
In order to run the files in vivado properly, ensure that the files are in .v format. 
After adding your design files and a Testbench, go to the Flow Navigator on the left and select Run Simulation > Run Behavioral Simulation. 
This opens the simulation layout where you can view the waveform window. Here, you’ll check if your signals (inputs and outputs) behave as expected over time. 
If the logic is flawed, you fix it here before wasting time on the later, more time-consuming steps.

Once your logic is verified, click Run Synthesis. During this phase, Vivado takes your high-level HDL code (Verilog/VHDL) and converts it into a gate-level netlist. 
It essentially translates your code into generic logic gates and registers. Once finished, you can open the Synthesized Design to view the Schematic, which shows a visual representation of how Vivado interpreted your code.

Once your synthesis is complete, clicking Run Implementation triggers the "Place and Route" phase, where Vivado maps your logic gates to specific physical locations on the FPGA fabric and optimizes 
the wiring between them to meet timing requirements. 

After implementation finishes without errors, selecting Generate Bitstream converts this physical layout into a .bit binary file that contains the configuration data for the chip. 
Finally, you use the Hardware Manager to load this bitstream onto your device, transforming your RTL description into functioning hardware.

ESP32S3 WVROOM upload instructions:
Run Arduino IDE with the following plugins:

Baud Rate 115200 for serial output and troubleshooting messages.
