# TP: Scheduling, Interrupciones y Threads

## Introducción

Este trabajo práctico extiende la base del esqueleto JOS para explorar tres grandes áreas del diseño de un sistema operativo moderno: el mecanismo de cambio de contexto, el scheduling dirigido por hardware, y el soporte para múltiples hilos de ejecución.

El TP se divide en cuatro partes de dificultad creciente. Las partes 1 y 2 deben completarse en ese orden antes de seguir con 3 y 4.

---

## Parte 1: Cambio de contexto

JOS mantiene un arreglo en memoria como PCB (Process Control Block), aunque llama *environment* a los procesos. De aquí en más se usarán las palabras proceso y environment como sinónimos.

Las funciones que reservan espacio, crean el espacio de direcciones virtuales y cargan el código del proceso ya están implementadas en `kern/env.c` (`env_alloc`, `env_setup_vm`, `load_icode`, `env_destroy`, `env_free`). No las modificarán en esta parte, pero es importante entender su flujo: en la Parte 3 se pedirán cambios puntuales en `env_alloc` y en la Parte 4 en `env_free`.

### De modo kernel a modo usuario

La función `env_run` (en `kern/env.c`) toma un proceso y lo ejecuta. Debe:

1. Actualizar `curenv` con el nuevo proceso.
2. Cambiar `env_status` a `ENV_RUNNING`.
3. Cargar el page directory del environment con `env_load_pgdir`.
4. Llamar a `context_switch` para restaurar el estado de CPU.

`context_switch` (en `kern/switch.S`) restaura todos los registros del `struct Trapframe` y usa `iret` para saltar a modo usuario (ring 3). Esta función **nunca hace return**.

### De modo usuario a modo kernel

El regreso a modo kernel ocurre mediante interrupciones. Los handlers están en `kern/trapentry.S`, generados con las macros `TRAPHANDLER` y `TRAPHANDLER_NOEC`. Todos desembocan en `_alltraps`, que debe completar el `struct Trapframe` en el stack y llamar a la función `trap`.

### Tarea

- Implementar `context_switch` en `kern/switch.S`. Usar `iret` al final.
- Completar `env_run` en `kern/env.c`.
- Implementar `_alltraps` en `kern/trapentry.S`.
- Modificar `kern/init.c` temporalmente para ejecutar un único proceso `user_hello`.

### Informe

Usar GDB para visualizar el cambio de contexto. Mostrar:
- El estado del stack al inicio de `context_switch`.
- Cómo cambia el stack instrucción a instrucción.
- Cómo se modifican los registros luego de ejecutar `iret`.
- El camino inverso: desde una syscall en user space hasta `trap` en el kernel.

---

## Parte 2: Round Robin preemptivo con timer

Compilar y correr con: `make qemu USE_RR=1`

Los tests automáticos (`make grade USE_RR=1`) validan el scheduler base: que los procesos alternen, no se pisen y terminen. El resto de esta parte (`sys_sleep` y las estadísticas) se valida a mano con `user/sleeptest.c` y con la salida del kernel.

### El timer como mecanismo de preemption

En la Parte 1, el kernel solo recupera el control cuando el proceso hace una syscall voluntariamente. Un scheduler real también debe poder **interrumpir** un proceso que no cede la CPU.

JOS ya tiene configurado el LAPIC timer para generar interrupciones periódicas (`IRQ_TIMER`). Al producirse, `trap_dispatch` en `kern/trap.c` llama a `lapic_eoi()` para confirmar la interrupción y luego a `sched_yield()`. Con esto, el scheduler puede reemplazar al proceso actual aunque este no haya llamado a `sys_yield`.

Existe un contador global `ticks`, definido en `kern/trap.c` y declarado en `kern/trap.h`, que debe incrementarse en cada interrupción de timer. Este contador permite medir el tiempo transcurrido desde el arranque del kernel.

### Round robin

`sched_yield` en `kern/sched.c` elige el próximo proceso a ejecutar. Con round robin:
- Se recorre el arreglo `envs` en forma circular, empezando justo después del último proceso ejecutado.
- Se elige el primer `ENV_RUNNABLE` encontrado.
- Si no hay ninguno y `curenv` sigue en `ENV_RUNNING`, se puede re-elegir.
- Si no hay nada que ejecutar, se llama a `sched_halt`.

### Syscall `sys_sleep`

Implementar la syscall `SYS_sleep` (stub en `kern/syscall.c`). El wrapper de usuario en `lib/syscall.c` y su prototipo en `inc/lib.h` ya están provistos.

```c
int sys_sleep(uint32_t n);
```

- Pone al proceso actual en `ENV_NOT_RUNNABLE` y guarda el tick de despertar en `env_sleep_until = ticks + n`.
- En cada interrupción de timer, después de incrementar `ticks`, llamar a `sched_wakeup_sleeping()` (stub en `kern/sched.c`) para despertar los procesos cuyo `env_sleep_until <= ticks`.
- La syscall no retorna hasta que el proceso sea despertado. Tener en cuenta que el valor de retorno de una syscall se escribe en `env_tf.tf_regs.reg_eax`: ver cómo resuelve esto `sys_ipc_recv`, que tiene el mismo problema.

Atención al efecto que tiene sleep sobre el final de la ejecución: un proceso dormido está en `ENV_NOT_RUNNABLE`, y `sched_halt` considera que no hay nada que ejecutar cuando ningún proceso está `ENV_RUNNABLE`, `ENV_RUNNING` ni `ENV_DYING`. ¿Qué ocurre si todos los procesos duermen al mismo tiempo? Hay que distinguir "no hay nada que ejecutar por ahora, pero el timer va a despertar a alguien" de "ya no queda ningún proceso vivo".

### Estadísticas

Al finalizar todos los procesos (en `sched_halt`), imprimir al menos:
- Número total de llamadas al scheduler.
- Número de veces que cada proceso fue ejecutado.
- Tiempo total de ejecución en ticks.

### Tarea

- Implementar round robin en `sched_yield` dentro del bloque `#ifdef SCHED_ROUND_ROBIN`.
- Incrementar `ticks` en el handler de `IRQ_TIMER` en `kern/trap.c`.
- Implementar `sched_wakeup_sleeping` en `kern/sched.c`.
- Implementar `sys_sleep` en `kern/syscall.c`.
- Inicializar `env_sleep_until` en `env_alloc` (`kern/env.c`): las entradas del arreglo `envs` se reciclan, así que un environment nuevo puede arrastrar el valor del anterior si no se resetea.
- Completar las estadísticas en `sched_halt`.
- Ejecutar `user/sleeptest.c` y verificar que los procesos duermen y despiertan correctamente. Para correrlo, reemplazar los `ENV_CREATE` de `kern/init.c` por `ENV_CREATE(user_sleeptest, ENV_TYPE_USER)` (el binario ya está declarado en `kern/Makefrag`).

### Informe

- Describir cómo el timer genera preemption: desde la interrupción hardware hasta el cambio de proceso.
- Mostrar con GDB un momento en que `sched_yield` es llamado desde el handler de timer (no desde una syscall).
- Comparar el comportamiento con y sin preemption ejecutando `user/spin.c` (un solo `ENV_CREATE`, el propio programa hace `fork()`). El hijo hace `while(1) /* nada */` sin ceder la CPU nunca: si el padre logra volver a correr después (`"Killing the child..."`), fue exclusivamente por preemption del timer, no por cooperación del hijo. Para desactivar la preemption y comparar, comentar momentáneamente la llamada a `sched_yield()` en el caso `IRQ_TIMER` de `trap_dispatch` — el kernel debería quedar trabado en el hijo para siempre.

---

## Parte 3: Scheduler con prioridades y aging

Compilar y correr con: `make qemu USE_PR=1`

Los tests automáticos (`make grade USE_PR=1`) validan que el scheduler siga siendo correcto con la política de prioridades activa; el comportamiento propio de las prioridades se valida con `user/priotest.c` y con los tests propios que se pidan más abajo.

### Motivación

Round robin asigna el mismo tiempo de CPU a todos los procesos. En la práctica, algunos procesos son más importantes o más livianos que otros. Un scheduler con prioridades permite reflejar eso.

Sin embargo, un scheduler puramente basado en prioridades puede causar **starvation**: los procesos de baja prioridad nunca obtienen CPU si siempre hay procesos de mayor prioridad disponibles. El mecanismo de **aging** mitiga esto aumentando gradualmente la prioridad efectiva de los procesos que llevan mucho tiempo esperando.

### Prioridades en el struct Env

El `struct Env` ya tiene los campos:
```c
uint32_t env_priority;   // prioridad del environment
uint32_t env_wait_ticks; // ticks acumulados esperando (para aging)
```

### Política de scheduling

En lugar de recorrer los procesos en orden circular, el scheduler debe elegir el
proceso listo con mayor **prioridad efectiva**, entendida como la prioridad base
del proceso más un bonus que crece con el tiempo que lleva esperando su turno:

```
prioridad_efectiva = env_priority + aging_bonus(espera acumulada)
```

### Syscalls de prioridad

Implementar:

```c
int sys_getpriority(envid_t envid);
int sys_setpriority(envid_t envid, uint32_t priority);
```

Reglas de seguridad:
- Un proceso **no puede aumentar su propia prioridad**.
- Un proceso puede **reducir su propia prioridad**.
- Un proceso puede modificar la prioridad de sus hijos directos (con cualquier valor).
- `sys_setpriority` con `envid == 0` refiere al proceso actual.

Como `sys_getpriority` usa su valor de retorno tanto para la prioridad como para los códigos de error (negativos), el rango de prioridades válidas debe ser acotado y no negativo. Definir ese rango (por ejemplo, un `MAX_PRIORITY` en `inc/env.h`) y documentarlo en el informe; también hace falta para la estadística de distribución de CPU por prioridad.

### Prioridad inicial y herencia en fork

- Todo proceso nuevo debe recibir una prioridad por defecto al crearse (modificar `env_alloc` o `env_create`).
- Cuando un proceso hace `fork`, el hijo hereda la prioridad del padre (o una fracción de ella, a criterio del alumno — justificar en el informe).

### Estadísticas

Además de las de la Parte 2, agregar:
- Historial de los últimos N procesos ejecutados (proceso + tick de inicio).
- Distribución de CPU por prioridad (cuántos ticks acumuló cada nivel de prioridad).

Tener en cuenta que `env_priority` es la prioridad **base** y el aging no la
modifica: el bonus vive en la prioridad efectiva, que se recalcula en cada
decisión. Unas estadísticas que sólo muestren `env_priority` se ven idénticas en
una corrida donde el aging fue decisivo y en una donde nunca actuó. Conviene
exponer también la prioridad efectiva, o la espera acumulada, para tener con qué
respaldar la demostración que pide la Tarea.

### Tarea

- Implementar `sched_yield` dentro del bloque `#ifdef SCHED_PRIORITIES`.
- Implementar `sys_getpriority` y `sys_setpriority` en `kern/syscall.c`.
- Modificar `env_alloc` en `kern/env.c` para asignar prioridad por defecto.
- Modificar el manejo de `fork` para heredar prioridad.
- Ejecutar `user/priotest.c` (vía `ENV_CREATE(user_priotest, ENV_TYPE_USER)` en
  `kern/init.c`).

Al terminar esta parte, con `USE_PR=1`:

- El scheduler elige por prioridad efectiva, existen `sys_getpriority` y
  `sys_setpriority` con las reglas de seguridad de arriba, todo proceso arranca
  con una prioridad por defecto y el hijo de un `fork` hereda la del padre.
- Las estadísticas incluyen el historial de decisiones y la distribución de CPU
  por prioridad.
- `user/priotest.c` corre y muestra que la política favorece a los procesos de
  alta prioridad (dejar un único `ENV_CREATE(user_priotest, ENV_TYPE_USER)`; el
  binario ya está declarado). Cuidado con la expectativa: con prioridades el
  proceso de mayor prioridad acapara la CPU hasta terminar, así que lo que
  corresponde ver es a los hijos terminando en orden descendente de prioridad, y
  no cuatro series de líneas intercaladas de forma pareja. Correr el mismo test
  con `USE_RR=1` da el contraste.
- Un test propio demuestra que el aging evita starvation. El escenario son dos
  procesos CPU-bound con prioridades distintas que nunca cedan la CPU por su
  cuenta —así lo único que puede sacársela es la preemption del timer— y hay que
  mostrar que el postergado obtiene CPU *antes* de que el otro termine. Los
  programas de usuario nuevos van en `user/` y hay que agregarlos a
  `KERN_BINFILES` en `kern/Makefrag`.
- El mismo test, anulando el bonus de aging, muestra la starvation contra la que
  se compara.

### Informe

- Describir la función de aging elegida, la constante que usa y por qué se
  eligió ese valor.
- Indicar el peor caso de espera de esa función, en cantidad de decisiones de
  scheduling, y explicar por qué eso alcanza para descartar starvation.
- Mostrar con resultados del scheduler que un proceso de baja prioridad
  eventualmente obtiene CPU, y con el contraste sin aging que sin ese mecanismo
  no lo obtenía.
- Explicar la política de herencia de prioridad en fork y su razonamiento.
- Comparar las estadísticas entre `USE_RR=1` y `USE_PR=1` para el mismo conjunto
  de procesos.

---

## Parte 4: Threads en espacio de usuario

Compilar y correr con `make qemu USE_RR=1` o `make qemu USE_PR=1`: los threads son independientes de la política de scheduling, y deben funcionar con ambas.

### Procesos vs Threads

Hasta ahora, cada `struct Env` tiene su propio page directory (`env_pgdir`): su espacio de direcciones es completamente separado del de otros procesos. Para comunicarse, los procesos deben usar IPC.

Un **thread** es una unidad de ejecución que comparte el espacio de direcciones de su proceso padre. Múltiples threads dentro del mismo proceso ven la misma memoria, lo que permite comunicación directa a través de variables compartidas, pero también requiere coordinación (sincronización) para evitar condiciones de carrera.

### Modelo de implementación

En JOS, un thread se representa como un `struct Env` con `env_type = ENV_TYPE_THREAD` cuyo `env_pgdir` apunta al **mismo** page directory que su proceso padre. El scheduler lo trata exactamente igual que a un proceso normal: tiene su propio `env_tf` (registros), su propia pila, y puede estar en cualquier estado (`ENV_RUNNABLE`, `ENV_NOT_RUNNABLE`, etc.).

La diferencia clave respecto a `fork` es:
- `fork` copia el page directory (copy-on-write).
- `sys_thread_create` **comparte** el page directory sin copiarlo.

### Syscall `sys_thread_create`

```c
envid_t sys_thread_create(void *entry, void *ustack_top);
```

- Crea un nuevo `struct Env` con `env_type = ENV_TYPE_THREAD`.
- Copia `env_pgdir` del proceso actual al nuevo env (sin duplicar las páginas).
- Inicializa `env_tf` del thread para que comience a ejecutar en `entry` con el stack en `ustack_top`.
- El thread hereda el `env_parent_id` del proceso creador.
- Retorna el `envid` del nuevo thread.

Cuando el proceso padre es destruido (`env_free`), todos sus threads deben ser destruidos también. Modificar `env_free` para contemplar esto.

Antes de tocar `env_free`, leerla con atención: tal como está, desmapea todo el espacio de usuario del environment y libera su page directory. Aplicada sobre un thread, que comparte el `env_pgdir` con su proceso, destruiría la memoria del proceso y del resto de los threads. Revisar también el refcount de la página del page directory (`pp_ref`, ver cómo lo maneja `env_setup_vm`) al compartirlo entre varios environments.

### Librería de usuario

El archivo `lib/thread.c` contiene el stub de `thread_create`:

```c
envid_t thread_create(void (*func)(void *), void *arg);
```

Esta función debe:
1. Alocar páginas para el stack del nuevo thread con `sys_page_alloc`.
2. Preparar el stack de modo que al retornar de `func` se llame a `exit()`.
3. Llamar a `sys_thread_create(func, stack_top)`.

El alumno decide la dirección donde alocar el stack y documenta el criterio. Tener en cuenta que la página `[USTACKTOP - PGSIZE, USTACKTOP)` ya está ocupada por el stack del thread principal (ver `load_icode` en `kern/env.c`), y que los stacks de los threads no deben solaparse entre sí.

### Consideraciones

- ¿Qué ocurre si dos threads modifican la misma variable simultáneamente? Mostrar un ejemplo concreto de condición de carrera en el informe (aunque no es necesario implementar sincronización).
- ¿Qué pasa con el page directory cuando un thread termina pero el padre sigue vivo? ¿Y al revés?
- ¿Puede un thread hacer `fork`? ¿Qué heredaría el hijo?
- La variable global `thisenv` la inicializa `libmain` (y la corrige `fork`) para el proceso, pero un thread arranca directamente en `entry` y comparte esa variable con el resto del proceso. ¿Qué implica eso para un thread que quiera conocer su propio `env_id`? ¿Cómo lo resolvería un sistema real?

### Tarea

- Agregar `ENV_TYPE_THREAD` al enum `EnvType` en `inc/env.h` (ya presente).
- Implementar `sys_thread_create` en `kern/syscall.c`.
- Modificar `env_free` en `kern/env.c` para destruir los threads hijos al destruir el proceso padre.
- Completar `thread_create` en `lib/thread.c` (el prototipo ya está en `inc/lib.h`).
- Completar `user/threadtest.c` (esqueleto provisto): crear al menos 3 threads que compartan una variable global e impriman su progreso.
- Demostrar que los threads ven la misma memoria (variable compartida modificada por un thread es visible para los demás).

### Informe

- Explicar la diferencia entre el page directory en `fork` vs `sys_thread_create`.
- Mostrar un ejemplo de condición de carrera entre dos threads.
- Describir cómo se decidió dónde alocar los stacks de los threads y por qué.
- Comparar el costo de creación de un thread vs un proceso (fork): ¿cuántas páginas se copian en cada caso?

---

## Apéndice: Comandos útiles

```bash
# Compilar y correr (Partes 1 y 2 con round-robin)
make qemu USE_RR=1

# Compilar con prioridades
make qemu USE_PR=1

# Correr tests automáticos
make grade USE_RR=1
make grade USE_PR=1

# GDB
make qemu-gdb USE_RR=1
# En otra terminal:
gdb -x .gdbinit
```

Los programas de usuario que se ejecutan al arrancar el kernel se eligen con `ENV_CREATE(user_<nombre>, ENV_TYPE_USER)` en `kern/init.c`. Cualquier programa nuevo en `user/` tiene que agregarse además a `KERN_BINFILES` en `kern/Makefrag` para que se compile y quede embebido en el kernel. Los tests `user/sleeptest.c`, `user/priotest.c` y `user/threadtest.c` ya están declarados ahí.

El entorno de desarrollo se puede levantar con Docker:

```bash
./dock build   # construir la imagen
./dock run     # entrar al container, con el repo montado en /sched
```

## Criterios de evaluación

| Parte | Puntaje |
|-------|---------|
| Parte 1: Cambio de contexto | 25% |
| Parte 2: Round robin + timer + sleep | 25% |
| Parte 3: Prioridades + aging + syscalls | 25% |
| Parte 4: Threads | 25% |

En todas las partes se evalúa tanto la implementación como el informe.
