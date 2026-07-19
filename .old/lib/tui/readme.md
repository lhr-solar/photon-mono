```
     ⣀⡠⠤⠤⠤⣀⡀    
  ⢀⡴⡏⠉⠉⠓⢦⡤⠞⠉⠳⣄  
 ⢠⠏ ⡇⣀⡤⠚⠁⠘⢦⣀⣠⠜⢧ 
 ⣾⣠⠴⠻⡅  ⢀⡠⠖⢻⡀ ⠘⡆
 ⣿⠁  ⣹⡤⠚⠉   ⣳⠴⠋⡇
 ⠸⣆⠴⠚⠁⠙⢦⢀⡠⠖⠋⠙⡆⡼ 
  ⠙⢷⣄⢀⡠⠖⠙⢤⡀ ⣸⠟⠁ 
    ⠉⠛⠲⠦⠤⠤⠛⠋⠁   
█████░░░░░░░░░░░
 1.00│ ╭────╮                ╭──
 0.60│─╯    ╰─╮            ╭─╯  
 0.20│        ╰╮         ╭─╯    
-0.20│         ╰╮       ╭╯      
-0.60│          ╰╮    ╭─╯       
-1.00│           ╰────╯         
     └──────────────────────────
Time  0.00                 15.00
1.00│                           
0.80│ █   █  █                  
0.60│ █   █  █  █               
0.40│ █   █  █  █               
0.20│ █   █  █  █   █           
0.00│ █   █  █  █   █  █  █   █ 
    └───────────────────────────
Bins 1.00                   8.00
```
- Entrypoint is `import tui;`
- Modules live in `lib/`
- Make integration fragment: `lib/tui.mk`
- Example build: `make`, `make run`, `make clean`
- Build artifacts and `compile_commands.json` are written to `.artifacts/`
- `clang++` with `-std=c++23`
