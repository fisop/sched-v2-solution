# Informe: Scheduling, Interrupciones y Threads

> **Nota sobre las capturas.** Las secciones marcadas con `[CAPTURA]` requieren
> pegar la salida de una corrida real (GDB o qemu). El análisis, los comandos
> exactos para obtenerlas y los valores que se esperan ver están completos; lo
> que falta es la salida textual de la ejecución.

---

## Parte 1: Cambio de contexto

### El stack al entrar a `context_switch`

`env_run` llama a `context_switch(&e->env_tf)`. Al entrar, el stack del kernel
tiene la forma habitual de una llamada cdecl:

```
esp + 0   dirección de retorno (dentro de env_run)
esp + 4   &e->env_tf          <- único argumento
```

La primera instrucción, `movl 4(%esp), %esp`, descarta ese stack y hace que esp
apunte al principio del `struct Trapframe`. A partir de ahí el stack *es* el
trapframe, y cada pop consume un campo en orden. Offsets (x86 de 32 bits,
`sizeof(struct Trapframe) == 68`):

| offset | campo | instrucción que lo consume |
|---|---|---|
| `0x00`–`0x1c` | `tf_regs` (edi, esi, ebp, oesp, ebx, edx, ecx, eax) | `popal` |
| `0x20` | `tf_es` + padding | `popl %es` |
| `0x24` | `tf_ds` + padding | `popl %ds` |
| `0x28` | `tf_trapno` | `addl $0x8, %esp` |
| `0x2c` | `tf_err` | idem |
| `0x30` | `tf_eip` | `iret` |
| `0x34` | `tf_cs` + padding | `iret` |
| `0x38` | `tf_eflags` | `iret` |
| `0x3c` | `tf_esp` | `iret` |
| `0x40` | `tf_ss` + padding | `iret` |

El orden no es una elección: es el formato que el hardware exige para `iret`.
Los campos que `iret` levanta tienen que quedar exactamente en el tope del
stack, y por eso hay que saltear `tf_trapno` y `tf_err` con el `addl $0x8`: son
información que el kernel usa para diagnosticar el trap, no parte del contrato
con el hardware.

`popal` restaura los ocho registros de propósito general en el orden inverso a
`pushal`, que es justamente el orden en que están declarados en `struct
PushRegs`. `reg_oesp` se descarta (`popal` ignora el valor que corresponde a
esp).

Código compilado (`objdump -d obj/kern/switch.o`):

```
00000000 <context_switch>:
   0:	8b 64 24 04    movl   0x4(%esp),%esp
   4:	61             popal
   5:	07             popl   %es
   6:	1f             popl   %ds
   7:	83 c4 08       addl   $0x8,%esp
   a:	cf             iret
```

`[CAPTURA]` Cómo obtener la evolución del stack instrucción por instrucción:

```gdb
break context_switch
run
# stack de la llamada: retorno + puntero al trapframe
x/2wx $esp
# el trapframe completo, ya como stack, después del primer movl
si
x/17wx $esp
# ir avanzando de a una instrucción y ver cómo sube esp
si          # popal
info registers
si          # popl %es
si          # popl %ds
si          # addl $0x8
x/5wx $esp  # eip, cs, eflags, esp, ss: lo que va a levantar iret
si          # iret
info registers
```

### Los registros después de `iret`

Para el primer environment (`user_hello`), los valores que deja `iret` son los
que `env_alloc` y `load_icode` prepararon en el trapframe:

- `eip` = entry point del ELF de usuario (`0x800020` con el layout por defecto).
- `cs` = `GD_UT | 3` = `0x1b`. Los dos bits bajos son el CPL: pasar de `0x08`
  (`GD_KT`) a `0x1b` **es** el cambio de ring 0 a ring 3.
- `eflags` = `0x200` (`FL_IF`): las interrupciones quedan habilitadas en modo
  usuario, que es lo que después permite la preemption por timer.
- `esp` = `USTACKTOP` = `0xeebfe000`, `ss` = `GD_UD | 3` = `0x23`.

Que `iret` cambie `cs`, `eip` y `esp` de forma atómica es la razón por la que se
usa: cualquier secuencia de instrucciones equivalente tendría un instante
intermedio con, por ejemplo, el CPL ya bajado a 3 pero el stack todavía apuntando
al del kernel.

### El camino inverso: de una syscall a `trap`

1. El programa de usuario llama a `sys_cputs`, que en `lib/syscall.c` ejecuta
   `int $0x30` (`T_SYSCALL` = 48).
2. El hardware busca la entrada 48 de la IDT, cargada en `trap_init` con
   `SETGATE(idt[T_SYSCALL], 0, GD_KT, &trap48, 3)`. El último parámetro (DPL 3)
   es lo que permite que un proceso de usuario dispare esta interrupción; el
   `GD_KT` es lo que garantiza que el handler corra en ring 0.
3. Como hay cambio de privilegio, el CPU carga esp0/ss0 del TSS y pushea, en el
   stack del kernel: `ss`, `esp`, `eflags`, `cs`, `eip`.
4. `trap48` (generado por `TRAPHANDLER_NOEC`) pushea un error code falso (0) y
   el número de trap (48), y salta a `_alltraps`.
5. `_alltraps` completa el trapframe: `pushl %ds`, `pushl %es`, `pushal`. El
   orden es el inverso al de los campos en memoria, porque el stack crece hacia
   abajo. Después carga `GD_KD` en `ds` y `es` (venían con los selectores de
   usuario) y hace `pushl %esp` para pasar el trapframe como argumento de
   `trap`.
6. `trap` copia el trapframe a `curenv->env_tf` y llama a `trap_dispatch`, que
   despacha a `syscall()` y deja el resultado en `tf->tf_regs.reg_eax`.

```
00000076 <_alltraps>:
  76:	1e             pushl  %ds
  77:	06             pushl  %es
  78:	60             pushal
  79:	b8 10 00 00 00 movl   $0x10,%eax     # GD_KD
  7e:	8e d8          movl   %eax,%ds
  80:	8e c0          movl   %eax,%es
  82:	54             pushl  %esp
  83:	e8 ..          call   trap
```

`[CAPTURA]`

```gdb
break trap48
break trap
run
# en trap48: ver los 5 valores que pusheó el hardware
x/5wx $esp
# en trap: el struct Trapframe completo que armó _alltraps
p *tf
p/x tf->tf_trapno    # 48
p/x tf->tf_cs        # 0x1b: el trap vino de ring 3
```

---

## Parte 2: Round Robin preemptivo con timer

### De la interrupción de hardware al cambio de proceso

El LAPIC está programado en modo periódico por `lapic_init` (`kern/lapic.c`),
con el vector `IRQ_OFFSET + IRQ_TIMER` (32). La cadena completa es:

1. El LAPIC dispara la interrupción 32. El proceso de usuario la recibe porque
   su `eflags` tiene `FL_IF` (lo puso `env_alloc`).
2. `idt[32]` apunta a `trap32`, con DPL 0: el hardware entra al kernel, cambia a
   la pila del kernel y pushea el estado del proceso interrumpido.
3. `trap32` → `_alltraps` → `trap` → `trap_dispatch`, igual que una syscall. La
   diferencia es que el proceso no pidió nada: fue interrumpido en cualquier
   instrucción.
4. En `trap_dispatch`, el caso `IRQ_TIMER`:
   - `lapic_eoi()` confirma la interrupción (si no, el LAPIC no manda más).
   - `ticks++`: el tick es la unidad de tiempo del kernel.
   - `sched_wakeup_sleeping()` mueve a `ENV_RUNNABLE` a los que terminaron de
     dormir, antes de decidir, así entran en esta misma vuelta.
   - `sched_yield()` elige el próximo proceso y no retorna.
5. `sched_yield` → `env_run` → `context_switch`, que restaura *otro* trapframe.
   El proceso interrumpido queda `ENV_RUNNABLE` con su estado completo guardado
   en `env_tf`, y va a retomar exactamente en la instrucción donde lo cortaron.

El punto conceptual: el kernel no "le pide" al proceso que ceda la CPU. El
proceso pierde la CPU sin enterarse, y la ilusión de continuidad la sostiene el
trapframe.

`[CAPTURA]` `sched_yield` llamado desde el timer y no desde una syscall — el
breakpoint condicional distingue los dos casos por el número de trap:

```gdb
break trap_dispatch if tf->tf_trapno == 32
run
# confirmar que el trap es el del timer y que venía de ring 3
p/x tf->tf_trapno
p/x tf->tf_cs
p ticks
# entrar hasta sched_yield y ver el backtrace: no hay ningún sys_yield
break sched_yield
continue
backtrace
```

El backtrace esperado es `sched_yield → trap_dispatch → trap → _alltraps`,
sin `syscall` ni `sys_yield` en el medio: eso es exactamente la evidencia de que
el cambio de contexto fue involuntario.

### `sys_sleep` y el valor de retorno

`sys_sleep` no puede retornar por el camino normal: pone al environment en
`ENV_NOT_RUNNABLE` y llama a `sched_yield()`, que no vuelve. El problema es que
el valor de retorno de una syscall lo escribe `trap_dispatch` en
`tf->tf_regs.reg_eax` *después* de que la función retorne, y a ese punto no se
llega nunca.

La solución es la misma que usa `sys_ipc_recv`: escribir el valor de retorno a
mano en el trapframe guardado antes de ceder la CPU.

```c
curenv->env_tf.tf_regs.reg_eax = 0;
sched_yield();
```

Cuando `sched_wakeup_sleeping` lo vuelva a poner `ENV_RUNNABLE` y el scheduler
lo elija, `context_switch` va a restaurar ese `eax` y en espacio de usuario la
syscall va a "retornar" 0, muchos ticks después de haberse llamado.

### Todos los procesos durmiendo: el caso que rompe `sched_halt`

`sched_halt` tenía una condición heredada de JOS: si ningún environment está
`ENV_RUNNABLE`, `ENV_RUNNING` ni `ENV_DYING`, asume que no queda trabajo y cae
en el monitor del kernel. Con `sys_sleep` eso pasa a ser falso: un proceso
dormido está `ENV_NOT_RUNNABLE`, así que si todos duermen a la vez el kernel
concluiría que la ejecución terminó y nunca los despertaría, aunque el timer
siga corriendo.

La condición corregida distingue tres situaciones:

- Hay algún environment listo o corriendo → se elige y se ejecuta.
- No hay ninguno listo, pero hay al menos uno dormido (`env_sleep_until != 0`) →
  se va al `sti; hlt`. Las interrupciones quedan habilitadas, así que el timer
  entra, incrementa `ticks` y despierta a quien corresponda.
- No queda ninguno vivo ni dormido → recién ahí terminó el trabajo: se imprimen
  las estadísticas y se entra al monitor.

Un environment `ENV_NOT_RUNNABLE` con `env_sleep_until == 0` (bloqueado
esperando un IPC que nunca va a llegar, por ejemplo) sigue contando como "no hay
nada que hacer", igual que en JOS original.

Un detalle relacionado: las entradas de `envs[]` se reciclan, y `env_alloc` no
limpiaba los campos nuevos. Si un environment heredara el `env_sleep_until` del
que ocupaba antes su slot, `sched_wakeup_sleeping` podría "despertarlo" mientras
está bloqueado en un IPC. Por eso `env_alloc` ahora lo pone en 0
explícitamente.

### Estadísticas

Se imprimen en `sched_halt`, en la rama donde ya no queda ningún environment.
Incluyen las llamadas al scheduler, los ticks totales, y por environment la
cantidad de veces que fue elegido y los ticks de CPU que acumuló. La atribución
de ticks se hace en cada decisión de scheduling: se le cobran al environment que
venía corriendo los ticks transcurridos desde que empezó.

Con más de una CPU la atribución es aproximada, porque `ticks` es global y dos
environments que corren en paralelo se reparten los mismos ticks de pared.

Salida real de `make qemu-nox USE_RR=1` con `ENV_CREATE(user_sleeptest,
ENV_TYPE_USER)` en `kern/init.c`:

```
[00000000] new env 00001000
[00001000] sleeptest starting
[00001000] new env 00001001
[00001000] new env 00001002
[00001001] child 0 sleeping for 5 ticks
[00001000] new env 00001003
[00001000] parent sleeping for 3 ticks
[00001002] child 1 sleeping for 10 ticks
[00001003] child 2 sleeping for 15 ticks
[00001000] parent awake
[00001000] exiting gracefully
[00001000] free env 00001000
[00001001] child 0 woke up!
[00001001] exiting gracefully
[00001001] free env 00001001
[00001002] child 1 woke up!
[00001002] exiting gracefully
[00001002] free env 00001002
[00001003] child 2 woke up!
[00001003] exiting gracefully
[00001003] free env 00001003
No runnable environments in the system!

=== estadisticas del scheduler ===
politica: round robin preemptivo
llamadas al scheduler (sched_yield): 25
ticks de timer desde el arranque: 16

ejecuciones por environment:
  [00001000] elegido 3 veces, 3 ticks de CPU, prioridad 8
  [00001001] elegido 2 veces, 5 ticks de CPU, prioridad 8
  [00001002] elegido 2 veces, 5 ticks de CPU, prioridad 8
  [00001003] elegido 2 veces, 3 ticks de CPU, prioridad 8

distribucion de CPU por prioridad (16 ticks atribuidos):
  prioridad  8: 16 ticks (100%)
```

Los tres hijos duermen tiempos distintos (5, 10 y 15 ticks) y despiertan en ese
mismo orden, incluidos los momentos en que los cuatro procesos (padre + 3
hijos) están dormidos a la vez y el kernel no se cuelga. Los números también
cierran entre sí: `3 + 5 + 5 + 3 = 16` ticks de CPU coincide exacto con los
`16 ticks atribuidos` de la distribución final.

### Preemption con y sin timer

Para aislar el efecto de la preemption sin la complejidad de un test de IPC,
se usó `user/spin.c` (ya provisto por el esqueleto, sin modificar): un hijo que
hace `while (1) /* nada */;` — cero syscalls, cero yields — y un padre que
cede la CPU 8 veces con `sys_yield()` y después mata al hijo.

**Con preemption** (`make qemu-nox USE_RR=1`, sin tocar nada más):

```
[00000000] new env 00001000
I am the parent.  Forking the child...
[00001000] new env 00001001
I am the parent.  Running the child...
I am the child.  Spinning...
I am the parent.  Killing the child...
[00001000] destroying 00001001
[00001000] free env 00001001
[00001000] exiting gracefully
[00001000] free env 00001000
No runnable environments in the system!

=== estadisticas del scheduler ===
politica: round robin preemptivo
llamadas al scheduler (sched_yield): 19
ticks de timer desde el arranque: 9

ejecuciones por environment:
  [00001000] elegido 10 veces, 1 ticks de CPU, prioridad 8
  [00001001] elegido 8 veces, 8 ticks de CPU, prioridad 8

distribucion de CPU por prioridad (9 ticks atribuidos):
  prioridad  8: 9 ticks (100%)
```

La línea `"I am the parent. Killing the child..."` es la prueba: como el hijo
nunca cede la CPU por su cuenta, la única forma de que el padre vuelva a
correr es que el timer se la haya sacado a la fuerza. El historial de
decisiones (no reproducido acá) muestra además una alternancia perfecta,
tick por tick, entre padre e hijo: cada `sys_yield()` del padre le da el
turno al hijo, y en el tick siguiente la preemption se lo devuelve al padre.

`[CAPTURA]` **sin preemption**: comentar la llamada a `sched_yield()` dentro
del caso `IRQ_TIMER` de `trap_dispatch` (dejando `lapic_eoi()`, `ticks++` y
`sched_wakeup_sleeping()`), recompilar y volver a correr. Se espera que la
salida se corte después de `"I am the child.  Spinning..."` y no avance
nunca más: el hijo queda como `curenv` para siempre, porque nada vuelve a
invocar al scheduler. Hay que matar QEMU a mano (`Ctrl-a`, `x`); no hay
`Killing the child`, ni `free env`, ni estadísticas. Recordar descomentar la
línea después de esta prueba.

### Nota sobre `user/fairness.c`

`fairness.c` no forma parte de la comparación anterior a propósito. Al
correrlo con la configuración que sugiere su propio comentario (`envs[0]` =
`user_idle`, `envs[1..3]` = tres instancias de `user_fairness`), un mismo
emisor gana sistemáticamente el slot de recepción del receptor, y el otro
nunca logra completar un envío — con o sin preemption, el resultado es
idéntico. La causa no es la política de scheduling: `sys_ipc_try_send` no
cede la CPU cuando el envío tiene éxito, y quién gana la carrera lo determina
el orden fijo de creación en `envs[]` (el proceso creado justo después del
receptor le gana siempre la carrera al siguiente). Es un buen ejemplo de que
un scheduler justo en tiempo de CPU no implica que la arbitración de un
recurso externo —acá, quién recibe el próximo IPC— también lo sea, pero no
es una medida de preemption y por eso se lo dejó fuera de la comparación
principal.

---

## Parte 3: Scheduler con prioridades y aging

### Rango de prioridades

Se definió en `inc/env.h`:

```c
#define ENV_PRIORITY_MIN 0
#define ENV_PRIORITY_MAX 15
#define ENV_PRIORITY_DEFAULT 8
```

El rango es acotado y no negativo por dos razones concretas: `sys_getpriority`
usa su valor de retorno tanto para la prioridad como para los códigos de error
(que son negativos), y la estadística de distribución de CPU por prioridad se
acumula en un arreglo indexado por prioridad. El valor por defecto está en el
medio del rango para que un proceso pueda tanto bajar el suyo como recibir uno
más alto de su padre.

### La función de aging

```c
static uint32_t
sched_effective_priority(struct Env *e)
{
	uint32_t bonus = e->env_wait_ticks / AGING_THRESHOLD;

	if (bonus > ENV_PRIORITY_MAX)
		bonus = ENV_PRIORITY_MAX;

	return e->env_priority + bonus;
}
```

con `AGING_THRESHOLD == 4`: cada 4 decisiones de scheduling que un environment
pasa esperando en la cola de listos, gana un punto de prioridad efectiva. El
elegido resetea su `env_wait_ticks` a 0; todos los demás `ENV_RUNNABLE` lo
incrementan. La unidad es la *decisión de scheduling*, no el tick de timer: el
contador se toca en `sched_yield`, que corre tanto en cada interrupción de timer
como en cada `sys_yield`. Para dos procesos CPU-bound las dos magnitudes
coinciden casi exactamente, porque ahí la única fuente de decisiones es el
timer, y eso es lo que hace que los ticks de las estadísticas sirvan para
verificar el peor caso.

**Peor caso de espera: 60 decisiones de scheduling.** `env_wait_ticks` crece de
forma monótona mientras el environment no sea elegido, así que su prioridad
efectiva también. En el peor escenario —un environment en `ENV_PRIORITY_MIN`
compitiendo contra uno en `ENV_PRIORITY_MAX` que nunca se bloquea— necesita un
bonus de 15 puntos para alcanzarlo, o sea
`ENV_PRIORITY_MAX * AGING_THRESHOLD` = 15 · 4 = 60 decisiones.

Y cuando empatan, gana el que estuvo esperando. Esto **no** sale del recorrido
circular: el barrido arranca en `ENVX(curenv) + 1`, así que ordena los empates
por índice de slot, no por antigüedad de la espera. Sale del desempate contra
`curenv`, que es el que importa acá: el environment que está corriendo no
aparece entre los candidatos del barrido (su `env_status` es `ENV_RUNNING`, no
`ENV_RUNNABLE`), se lo considera aparte, y sólo conserva la CPU con prioridad
efectiva **estrictamente** mayor. Con el `>=` en su lugar, el empate lo ganaría
siempre el que ya está corriendo y la starvation volvería intacta a pesar del
aging.

La cota en `ENV_PRIORITY_MAX` no es necesaria para evitar starvation (un bonus
sin techo también funciona), pero mantiene la prioridad efectiva en un rango
conocido y hace que el peor caso de espera sea una constante calculable en lugar
de depender de cuánto se dejó correr el sistema.

Sobre la elección de 4: el umbral está apretado por los dos lados, y los dos
límites son observables en los tests de esta parte.

- Hacia arriba, el peor caso crece linealmente con el umbral. Con 8 —el primer
  valor que se probó— son 120 decisiones, más de lo que produce una corrida
  corta: `user/agingtest.c` terminaba entero en ~99 ticks, de los cuales apenas
  la mitad ocurrían con el proceso de prioridad alta todavía vivo. El bonus del
  postergado llegaba a ~6 sobre los 15 que necesitaba y no corría nunca. La
  garantía anti-starvation seguía siendo cierta, pero era inobservable, que para
  un mecanismo cuyo único propósito es ese equivale a no tenerlo.
- Hacia abajo, el umbral es lo que un punto de prioridad "vale" en decisiones de
  scheduling. Con 1 o 2, los cuatro hijos de `user/priotest.c` —que están
  separados por un punto— se reordenan tan rápido que la prioridad base deja de
  ser visible en la salida. No es que la política se vuelva round robin (para
  prioridades *iguales* la alternancia es de a una decisión con cualquier
  umbral), pero sí se achica la ventaja de cada nivel: un proceso postergado por
  una diferencia de `g` puntos recibe la CPU una vez cada `g * AGING_THRESHOLD`
  decisiones, así que el umbral es directamente el factor de esa proporción.

Con 4, el peor caso queda en 60 decisiones —alcanzable con holgura por un test
CPU-bound razonable— y una diferencia de un solo punto de prioridad todavía se
traduce en 4 turnos consecutivos, que se ven en la salida de `priotest`.

### Reglas de seguridad de las syscalls

Las dos syscalls usan `envid2env(envid, &e, true)`, que ya resuelve `envid == 0`
como el environment actual y sólo autoriza el propio environment o un hijo
directo; cualquier otro caso devuelve `-E_BAD_ENV`. Sobre eso,
`sys_setpriority` agrega la regla asimétrica:

```c
if (e == curenv && priority > e->env_priority)
	return -E_INVAL;
```

Un proceso puede bajar su propia prioridad pero no subirla. Sobre un hijo
directo puede fijar cualquier valor, y eso no es un agujero: un padre no gana
CPU dándosela a un hijo, y como el hijo nunca puede exceder lo que el padre ya
tenía, no hay forma de escalar.

### Herencia en `fork`

Implementada del lado del kernel, en `sys_exofork`:

```c
newenv->env_priority = curenv->env_priority;
```

El hijo hereda el valor completo, como el nice value en POSIX. La alternativa
—darle una fracción— existe para evitar que un proceso "cultive" prioridad
forkeando, pero acá no hace falta: nadie termina con más prioridad de la que ya
tenía el padre. Para escalar habría que aumentar la propia, que es justamente lo
que `sys_setpriority` prohíbe. Y hacerlo en el kernel en lugar de en
`lib/fork.c` cubre a todos los usuarios de `sys_exofork` (`fork`, `dumbfork`) sin
depender de que cada uno se acuerde.

Los procesos que crea el kernel con `env_create` arrancan en
`ENV_PRIORITY_DEFAULT`, porque no tienen padre del cual heredar.

### Demostración de que el aging funciona

`user/agingtest.c` crea dos procesos CPU-bound —uno en `ENV_PRIORITY_MAX`, otro
en `ENV_PRIORITY_MIN`— que nunca llaman a `sys_yield`: la única forma de que
alternen es la preemption. El padre se saca del medio durmiendo con `sys_sleep`
en lugar de cediendo la CPU, para no competir en la cola de listos.

El dimensionamiento del test es la parte delicada, y la constante que lo gobierna
es `NROUNDS`, no `BUSY_ITERATIONS`. Llamando `K = ENV_PRIORITY_MAX *
AGING_THRESHOLD` = 15 · 4 = 60 al peor caso de espera, y aprovechando que
ninguno de los dos workers hace syscalls —con lo cual toda decisión viene de un
tick de timer y cada turno dura exactamente un tick—, la relación de CPU entre
los dos es de `K` a 1. De ahí salen dos condiciones:

1. "alta" tiene que seguir viva más de `K` decisiones, o "baja" no recibe ni un
   turno. Su vida es `NROUNDS · r` decisiones, con `r` = ticks que tarda una
   ronda.
2. "baja" sólo acumula `1/K` de la CPU, así que antes de que "alta" termine
   completa `(NROUNDS · r) / K / r` = `NROUNDS / K` rondas, y por lo tanto
   imprime esa cantidad de líneas. **La `r` se cancela**: agrandar
   `BUSY_ITERATIONS` alarga las rondas de los dos procesos por el mismo factor y
   no mueve ese número en absoluto.

La segunda condición es la que la primera versión del test no cumplía, y es un
error instructivo: con `NROUNDS = 20` y `K = 120` (el umbral era 8), "baja"
completaba 1/6 de ronda y no imprimía nada, y ninguna cantidad de trabajo extra
por ronda podía haberlo arreglado. Bajar el umbral a 4 sólo llevó la fracción a
1/3: seguía siendo menos de una ronda. Lo que lo resuelve es `NROUNDS = 200`, que
con `K = 60` da ~3 líneas de "baja" antes de que "alta" termine; y con
`BUSY_ITERATIONS = 3_000_000` una ronda tarda ~0,74 ticks, lo que deja la vida de
"alta" en ~148 decisiones contra las 60 necesarias, cubriendo la condición 1 con
holgura.

`[CAPTURA]` salida de `make qemu-nox USE_PR=1` con
`ENV_CREATE(user_agingtest, ENV_TYPE_USER)`. Lo que hay que ver: líneas del
worker "baja" intercaladas *antes* de que el worker "alta" termine sus 200
rondas (unas 3, por la cuenta de arriba), y en las estadísticas finales un
"bonus maximo de aging" de 15 para "baja" —la prueba de que su bonus saturó y por
eso pudo ganar— contra 0 para "alta", que nunca tuvo que esperar. Para el contraste, comentar el `+ bonus` en
`sched_effective_priority` y volver a correr: ahí "baja" no imprime nada hasta
que "alta" termina, y su bonus máximo queda en 0. Eso es la starvation.

Vale la pena notar por qué el bonus máximo tuvo que agregarse a las
estadísticas: `env_priority` es la prioridad *base* y el aging no la modifica
nunca —el bonus vive en la efectiva, que se recalcula en cada decisión y se
descarta—, así que unas estadísticas que sólo muestren `env_priority` se ven
exactamente iguales en una corrida donde el aging fue decisivo y en una donde no
actuó jamás. Por eso `sched_run` recibe la prioridad efectiva desde el llamador
(el policy ya reseteó el contador de espera cuando llega ahí) y la registra
tanto en el historial como en el máximo por environment.

`user/priotest.c` cubre el otro lado, el de que la política favorece a los
procesos de alta prioridad. El detalle de diseño que lo hace funcionar es que
los cuatro hijos reciben prioridades *por debajo* de la del padre: `fork` deja
al hijo `ENV_RUNNABLE` con la prioridad heredada y el padre le asigna la real
recién cuando `fork` ya retornó, así que con los hijos por encima el primero lo
preemptaría y correría hasta terminar antes de que el padre alcanzara a crear al
siguiente —nunca habría dos hijos compitiendo a la vez y el test no compararía
nada—. Quedando por encima de todos, el padre crea el conjunto completo y
después los larga a todos juntos bajando su propia prioridad por debajo de la de
ellos, que es justamente la operación que las reglas de seguridad permiten. Los
hijos, por su parte, esperan a que su prioridad cambie antes de arrancar, para no
llegar a correr con la heredada en la ventana entre `fork` y `sys_setpriority`.

`[CAPTURA]` salida de `user/priotest.c` con `USE_PR=1`. Lo que hay que ver: los
cuatro hijos terminando en orden descendente de prioridad, con alguna línea
suelta de los de prioridad más baja intercalada en el medio (ése es el aging
actuando sobre diferencias de un punto, cada 4 decisiones). Además verifica las
dos reglas de seguridad: el intento de subir la propia prioridad falla con
`-E_INVAL` y el de bajarla devuelve 0.

### Comparación de estadísticas entre `USE_RR=1` y `USE_PR=1`

`[CAPTURA]` correr `user/priotest.c` en los dos modos —sin tocar nada más que
el flag— y pegar las dos salidas de estadísticas.

Lo que se espera: en round robin los cuatro hijos avanzan a la par y sus líneas
salen intercaladas de forma pareja, aunque tengan prioridades distintas —la
política simplemente no las mira, y los ticks se reparten de forma
aproximadamente uniforme entre los procesos listos—. La distribución por
prioridad se sigue llenando (las syscalls de prioridad existen con las dos
políticas y el test las usa igual), pero muestra un reparto plano entre niveles
en lugar de una concentración. Con prioridades, en cambio, los hijos terminan en
orden descendente de prioridad, los ticks se concentran en los niveles altos, y
la fracción que reciben los niveles bajos es exactamente lo que aporta el aging:
sin él sería cero.

---

## Parte 4: Threads en espacio de usuario

### El page directory: `fork` vs `sys_thread_create`

Es la única diferencia estructural entre un proceso y un thread, y está en una
línea:

```c
/* fork (vía env_alloc → env_setup_vm): page directory nuevo */
e->env_pgdir = (uint32_t *) page2kva(p);
memcpy(e->env_pgdir, kern_pgdir, PGSIZE);

/* sys_thread_create: el mismo page directory del creador */
t->env_pgdir = curenv->env_pgdir;
pa2page(PADDR(t->env_pgdir))->pp_ref++;
```

`sys_thread_create` empieza llamando a `env_alloc`, que ya le crea al nuevo
environment un page directory propio. Hay que devolverlo (`page_decref`) antes
de apuntar al del creador: si no, esa página queda perdida.

El `pp_ref++` no es un detalle: a partir de ahí el page directory está
referenciado por dos environments. Sin el incremento, el primero de los dos en
morir liberaría la página que el otro sigue usando. Es el mismo motivo por el
que `env_setup_vm` mantiene `pp_ref` para el pgdir, y el propio comentario de
JOS lo advierte.

Del scheduler para arriba, un thread es un environment como cualquier otro:
tiene su `env_tf`, su estado, su prioridad, y `sched_yield` lo elige con las
mismas reglas. Toda la diferencia está en el espacio de direcciones.

### `env_free` en los dos sentidos

`env_free` desmapea todo el espacio de usuario del environment y libera su page
directory. Aplicado tal cual a un thread, destruiría la memoria del proceso y de
todos los demás threads. Por eso hay dos cambios simétricos:

- **Cuando muere un thread**, no toca el espacio de direcciones: sólo suelta su
  referencia al pgdir con `page_decref`. El proceso sigue vivo y su memoria
  intacta.
- **Cuando muere el proceso**, primero destruye a todos sus threads (los
  `ENV_TYPE_THREAD` cuyo `env_parent_id` es él) y después desarma el espacio de
  direcciones. El orden importa: cada thread suelta su referencia primero, así
  el `pp_ref` del pgdir llega a cero justo cuando lo libera el proceso.

Todos los threads cuelgan del proceso dueño del espacio de direcciones, incluso
los que crea otro thread (`sys_thread_create` resuelve el dueño mirando si el
creador es a su vez un thread). Si colgaran del thread creador, un thread
intermedio que muere dejaría "huérfanos" que el proceso no encontraría al morir.

Queda una limitación heredada del diseño de JOS: si un thread está corriendo en
otra CPU cuando muere el proceso, `env_destroy` lo marca `ENV_DYING` y recién se
libera en su próximo trap. En esa ventana el proceso ya desmapeó las páginas de
usuario, así que el thread va a fallar y morir. El `pp_ref` garantiza que al
menos la página del page directory no se reutilice mientras eso pasa.

Lo que **no** hace `env_free` es liberar el stack del thread: esas páginas las
pidió `thread_create` en espacio de usuario y es responsabilidad del usuario
liberarlas (o del `env_free` del proceso, que las desmapea junto con todo lo
demás). Un thread que termina deja su stack mapeado.

### Dónde van los stacks de los threads

El stack del thread principal ocupa `[USTACKTOP - PGSIZE, USTACKTOP)`, así que
los de los threads van por debajo, en orden descendente, con una página sin
mapear entre uno y otro:

```
USTACKTOP        ┌──────────────────────┐
                 │ stack thread principal│  1 página
USTACKTOP - 1P   ├──────────────────────┤
                 │ (guarda, sin mapear) │
USTACKTOP - 2P   ├──────────────────────┤
                 │ stack thread 0       │
USTACKTOP - 3P   ├──────────────────────┤
                 │ (guarda, sin mapear) │
USTACKTOP - 4P   ├──────────────────────┤
                 │ stack thread 1       │
USTACKTOP - 5P   └──────────────────────┘
```

Dos criterios detrás de esto:

1. **Cerca del stack principal, hacia abajo.** Es la zona del espacio de
   direcciones que ya está reservada para stacks y que está libre: el heap y el
   código del programa viven muy por debajo, arriba de `UTEXT`.
2. **Una página de guarda entre stacks.** Es lo que convierte un desborde de
   stack en un page fault en lugar de una corrupción silenciosa del stack del
   thread vecino. Cuesta una página de espacio virtual, que no es un recurso
   escaso, y no cuesta memoria física porque no está mapeada.

El error a evitar es arrancar en `USTACKTOP - PGSIZE`: esa página es el stack del
thread que está llamando a `thread_create`, y pedirla con `sys_page_alloc` la
remapea a una página nueva y en blanco, volando el stack del proceso en el medio
de la llamada.

`thread_create` arma además el stack para que la convención de llamada de x86 se
cumpla al entrar a `func`: en el tope quedan la dirección de retorno y el
argumento.

```c
sp = (uint32_t *) top;
*(--sp) = (uint32_t) (uintptr_t) arg;   /* esp + 4 al entrar a func */
*(--sp) = (uint32_t) (uintptr_t) exit;  /* esp + 0: dirección de retorno */
```

Poner `exit` como dirección de retorno es lo que hace que el thread termine solo
cuando `func` hace `return`: el `ret` de `func` salta a `exit`, que llama a
`sys_env_destroy(0)` y no vuelve.

### Condición de carrera

`user/threadtest.c` la muestra en dos fases. La primera incrementa
`shared_counter` y demuestra memoria compartida: el total final es exactamente
`NTHREADS * NITER`, y cada thread ve los incrementos de los demás. Con `fork` el
total quedaría en 0, porque cada hijo se llevaría su propia copia de la página.

La segunda fase exhibe la carrera. `race_counter++` no es atómico: compila a
leer, sumar, escribir. Si dos threads leen el mismo valor antes de que
cualquiera escriba, uno de los dos incrementos se pierde. Para que la pérdida
sea visible siempre y no dependa de dónde caiga la interrupción de timer, el
test separa la lectura de la escritura a propósito:

```c
int tmp = race_counter;
sys_yield();
race_counter = tmp + 1;
```

El resultado es que `race_counter` termina muy por debajo de `NTHREADS * NITER`,
mientras `shared_counter` da exacto. La diferencia entre los dos números es la
cantidad de actualizaciones perdidas.

`[CAPTURA]` salida de `make qemu USE_RR=1` con
`ENV_CREATE(user_threadtest, ENV_TYPE_USER)`.

Sin la separación explícita la carrera igual existe, pero es intermitente: la
ventana entre la lectura y la escritura es de pocas instrucciones y hace falta
que la interrupción de timer caiga justo ahí.

### Costo de crear un thread vs un proceso

Contando páginas físicas que se tocan:

| | `fork` | `sys_thread_create` |
|---|---|---|
| page directory | 1 página nueva, copiada de `kern_pgdir` | 0: comparte el del proceso |
| page tables | 1 por cada `PTSIZE` de espacio mapeado, creadas por `duppage` | 0: comparte |
| páginas de datos | 0 al momento del fork (se marcan copy-on-write), pero cada escritura posterior copia una página | 0 |
| stack de exception | 1 página (`UXSTACKTOP`), alocada fresca | 0 |
| stack propio | ya viene en el COW | 1 página |

Un `fork` en JOS arranca con al menos 3 páginas nuevas (pgdir, una page table y
el UXSTACK) y va pagando una página por cada página del padre que alguna de las
dos partes escriba. Un thread cuesta exactamente **una** página: su stack. Y en
el camino se ahorra todo el recorrido de `duppage` sobre el espacio de
direcciones del padre, que es lineal en la cantidad de páginas mapeadas.

Ese es el argumento de fondo a favor de los threads como unidad de concurrencia:
crear uno es O(1) en páginas y no requiere recorrer el espacio de direcciones.
La contrapartida es la de la segunda fase de `threadtest`: la memoria compartida
sin sincronización no es correcta, y el costo que se ahorra en la creación
reaparece como complejidad en la coordinación.

### Preguntas de las consideraciones

**¿Qué pasa con el page directory cuando un thread termina pero el padre sigue
vivo? ¿Y al revés?** Es lo que resuelve el refcount. Si termina el thread, sólo
baja `pp_ref` y el espacio de direcciones queda intacto para el proceso. Si
termina el proceso, primero destruye a sus threads y después desarma el espacio;
el pgdir se libera cuando el último `pp_ref` cae.

**¿Puede un thread hacer `fork`?** Sí: `sys_exofork` no distingue el tipo del
llamador. El hijo sería un proceso completo con una copia (copy-on-write) del
espacio de direcciones compartido, tal como estaba en ese instante. Pero hereda
sólo el thread que llamó: los demás threads no existen en el hijo, y si alguno
tenía tomado un recurso compartido —o estaba a mitad de un read-modify-write—
el hijo arranca con esa estructura en un estado intermedio y sin nadie que la
termine. Es exactamente el problema que en POSIX motiva `pthread_atfork` y la
regla de que sólo se llame a funciones async-signal-safe entre el `fork` y el
`exec`.
