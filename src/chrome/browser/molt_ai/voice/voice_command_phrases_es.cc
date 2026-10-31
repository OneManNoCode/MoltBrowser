// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Spanish: the direct-command patterns (voice_command_phrases.h has the
// syntax) and the command reference. Order matters: the first pattern
// that takes the whole utterance wins. Words are folded (no accents, "ñ"
// as "n"). Verbs come as the user says them to a machine: the familiar
// imperative ("abre"), the formal one ("abra") and the infinitive
// ("abrir"); the polite openings ("¿puedes ...?", "quiero ...") are
// dropped before these are tried (spoken_phrases_es.cc).

#include <iterator>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_command_phrases.h"

namespace molt_ai {
namespace voice {

namespace {

using K = VoiceCommandKind;
using C = PatternContext;
using V = PatternVariant;

constexpr CommandPattern kPatterns[] = {
    // --- Ways out: always first -------------------------------------------
    {K::kCancel,
     "(cancela|cancelar|cancele|cancelalo|anula|anular|anulalo|olvidalo|"
     "olvidate|olvida eso|da igual|no importa|dejalo|dejalo estar|"
     "cancela eso)"},
    {K::kStopSpeaking,
     "(deja de|para de|deje de|pare de|dejar de) (hablar|leer|leerme|"
     "hablarme|leer en voz alta)"},
    {K::kStopSpeaking,
     "(callate|calla|silencio|basta|ya basta|chis|shh|suficiente)"},
    {K::kStopLoading,
     "(para|deten|detener|parar|pare) la carga [de la pagina]"},
    {K::kStopLoading, "(deja de|para de|deje de) cargar [la pagina]"},
    {K::kPause,
     "(para|deten|detener|parar|pare) (el video|la musica|la cancion|la "
     "reproduccion|la pelicula|el audio)"},
    {K::kStop,
     "(para|parar|pare|detente|deten|detenlo|detener|alto|stop|paralo) "
     "[ya|todo|eso|ahora]"},

    // --- Voice mode itself ------------------------------------------------
    {K::kVoiceModeOff,
     "(desactiva|desactivar|desactive|apaga|apagar|apague|cierra|cerrar|"
     "sal del|salir del|termina|terminar) [el] (modo de voz|modo voz|control "
     "por voz|control de voz)"},
    {K::kVoiceModeOff,
     "(modo de voz|modo voz|control por voz) (desactivado|apagado|off)"},
    {K::kVoiceModeOff, "(deja de|para de|deje de) (escuchar|escucharme)"},
    {K::kVoiceModeOn,
     "(activa|activar|active|enciende|encender|encienda|inicia|iniciar|"
     "entra en|entrar en) [el] (modo de voz|modo voz|control por voz|control "
     "de voz)"},
    {K::kVoiceModeOn,
     "(modo de voz|modo voz|control por voz) (activado|encendido|on)"},
    {K::kVoiceModeOn,
     "(empieza|empezar|comienza|comenzar) a (escuchar|escucharme)"},

    // --- Corrections and answers ----------------------------------------
    {K::kCorrection, "no dije eso (dije|quise decir) {text}"},
    {K::kCorrection,
     "no (eso no|esta mal|mal|te equivocaste) (dije|quise decir) {text}"},
    {K::kCorrection,
     "[no] (dije|he dicho|quise decir|queria decir|pedi) {text}"},
    {K::kYes,
     "(si|sip|claro|vale|ok|okay|de acuerdo|correcto|confirmo|confirmar|"
     "confirma|afirmativo|por supuesto|desde luego|exacto|eso es|hazlo|"
     "adelante|dale|venga|seguro|perfecto|esta bien|muy bien|claro que si|"
     "si claro) [hazlo|adelante|eso es|confirmo|por favor|claro|es "
     "correcto|si]",
     C::kYesNo},
    {K::kNo,
     "(no|nop|negativo|para nada|de ninguna manera|ni hablar|incorrecto|"
     "mal) [no|no lo hagas|cancela|cancelar|eso no|ese no|no es eso|no "
     "gracias]",
     C::kYesNo},
    {K::kNo, "(no lo hagas|no hagas eso|mejor no)", C::kYesNo},
    {K::kUndo,
     "(deshaz|deshacer|deshaga|deshazlo) [eso|lo ultimo|la ultima "
     "accion|el ultimo cambio|lo que hiciste]"},
    {K::kRepeat,
     "(repite|repetir|repita|repitelo|repitemelo|otra vez|de nuevo|como|"
     "que dijiste|que has dicho|perdon|perdona|disculpa|mande|que|dilo otra "
     "vez|dimelo otra vez|dilo de nuevo|puedes repetir)"},
    {K::kHelp,
     "(ayuda|ayudame|necesito ayuda|que puedo decir|que puedo hacer|que "
     "puedes hacer|que digo|que ordenes hay|que comandos hay|muestra los "
     "comandos|muestrame los comandos|lista de comandos|comandos|ordenes|"
     "como funciona esto|como funciona)"},
    {K::kWhereAmI,
     "(donde estoy|donde estamos|en que (pagina|sitio|web|pestana) "
     "(estoy|estamos)|que (pagina|sitio|web) es (esta|esto|este))"},

    // --- Speech -----------------------------------------------------------
    {K::kSpeakFaster,
     "(habla|hablar|hable|lee|leer|lea) (mas rapido|mas deprisa|"
     "rapido|deprisa)"},
    {K::kSpeakSlower,
     "(habla|hablar|hable|lee|leer|lea) (mas despacio|mas lento|"
     "despacio|lento|mas lentamente)"},
    {K::kVerbosity,
     "(se breve|modo breve|breve|respuestas breves|respuestas cortas|"
     "respuestas mas cortas|detalle breve)",
     C::kAlways, V::kBrief},
    {K::kVerbosity,
     "(detalle normal|nivel de detalle normal|detalle por defecto)", C::kAlways,
     V::kNormal},
    {K::kVerbosity,
     "(detalle completo|todo el detalle|maximo detalle|modo detallado|se "
     "detallado|cuentamelo todo|dimelo todo)",
     C::kAlways, V::kFull},
    {K::kVerbosity, "(mas detalle|mas detalles|di mas|mas detallado)",
     C::kAlways, V::kMore},
    {K::kVerbosity, "(menos detalle|menos detalles|di menos|menos detallado)",
     C::kAlways, V::kLess},

    // --- Routines (learning) ----------------------------------------------
    // What was learned, forgotten: before "olvida {name}", which would take
    // these as a routine's name.
    {K::kForgetLearning,
     "(olvida|borra|elimina|olvidate de) todo [lo] que (aprendiste|has "
     "aprendido|sabes) [de mi|sobre mi]"},
    {K::kForgetWord,
     "(olvida|borra|elimina|quita) la (palabra|correccion|frase) {name}"},
    {K::kForgetWord, "(olvida|borra|elimina|quita) el nombre {name}"},
    {K::kListRoutines, "(cuales|que) son mis rutinas"},
    {K::kListRoutines, "que rutinas (tengo|hay|he guardado)"},
    {K::kListRoutines, "(lista|lee|leeme|dime|di) [todas] (las|mis) rutinas"},
    {K::kListRoutines, "(mis rutinas|rutinas)"},
    {K::kRememberRoutine,
     "(recuerda|guarda|memoriza) (esto|eso) como [rutina|una rutina] "
     "[llamada] {name}"},
    {K::kRememberRoutine,
     "(recuerdalo|guardalo|memorizalo) como [rutina|una rutina] [llamada] "
     "{name}"},
    {K::kRememberRoutine, "guarda esta rutina como {name}"},
    {K::kForgetRoutine,
     "(olvida|borra|elimina|quita) [la|mi] rutina [llamada] {name}"},
    {K::kForgetRoutine, "(olvida|olvidate de) {name}"},
    {K::kRunRoutine,
     "(ejecuta|ejecutar|lanza|inicia|haz|pon|corre|arranca) [la|mi] rutina "
     "[llamada] {name}"},
    {K::kRunRoutine, "(ejecuta|ejecutar|lanza|corre) {name}"},

    // --- Dictation --------------------------------------------------------
    {K::kNewLine,
     "(nueva linea|linea nueva|salto de linea|siguiente linea|nuevo "
     "parrafo|punto y aparte)",
     C::kTextField},
    {K::kDeleteLastWord,
     "(borra|borrar|elimina|eliminar|quita|quitar|deshaz) [la] ultima "
     "palabra",
     C::kTextField},
    {K::kDeleteLastWord, "borra esa palabra", C::kTextField},
    {K::kClearField,
     "(borra|borrar|vacia|vaciar|limpia|limpiar|elimina|eliminar) (el|este|"
     "la|esta) (campo|cuadro|texto|cuadro de texto|cuadro de busqueda|"
     "entrada|casilla)",
     C::kTextField},
    {K::kClearField,
     "(borra|borralo|borrar|limpia|limpialo|vacia|vacialo) [todo|eso]",
     C::kTextField},
    {K::kReadBack, "(lee|leeme|leer) lo que (he escrito|escribi|he dicho|dije)",
     C::kTextField},
    {K::kReadBack, "(leelo|leemelo) [otra vez|de nuevo]", C::kTextField},
    {K::kReadBack, "que (he escrito|escribi)", C::kTextField},
    {K::kSubmit,
     "(envia|enviar|envialo|manda|mandar|mandalo|envia el mensaje|busca|"
     "buscar|listo|hecho|intro|enter|ya esta)",
     C::kTextField},
    {K::kSubmit, "(pulsa|presiona|dale a) [la tecla] (intro|enter|entrar)"},
    {K::kSubmit, "(envia|enviar|manda|mandar) [el] formulario"},
    {K::kType, "(escribe|escribir|escriba|teclea|teclear|dicta|dictar) {text}"},

    // --- Scrolling (before "vuelve": "vuelve al principio") --------------
    {K::kScrollToTop,
     "[ve|ir|vuelve|volver|sube|subir|desplazate|vete] (al principio|al "
     "inicio|arriba del todo|hasta arriba) [de la pagina]"},
    {K::kScrollToTop, "(principio|inicio) de [la] pagina"},
    {K::kScrollToBottom,
     "[ve|ir|baja|bajar|desplazate|vete] (al final|abajo del todo|hasta "
     "abajo|hasta el final) [de la pagina]"},
    {K::kScrollToBottom, "final de [la] pagina"},
    {K::kScrollDown, "(pagina abajo|avance de pagina)", C::kAlways,
     V::kPageStep},
    {K::kScrollUp, "(pagina arriba|retroceso de pagina)", C::kAlways,
     V::kPageStep},
    {K::kScrollDown,
     "(baja|bajar|baje|desplazate hacia abajo|desplaza hacia abajo|"
     "desplazar hacia abajo|ve hacia abajo|mas abajo|abajo|sigue bajando|"
     "baja mas) {pages?}"},
    {K::kScrollUp,
     "(sube|subir|suba|desplazate hacia arriba|desplaza hacia arriba|"
     "desplazar hacia arriba|ve hacia arriba|mas arriba|arriba|sube mas) "
     "{pages?}"},
    {K::kScrollLeft,
     "(desplazate|desplaza|desplazar|ve|mueve) (a la|hacia la) izquierda"},
    {K::kScrollRight,
     "(desplazate|desplaza|desplazar|ve|mueve) (a la|hacia la) derecha"},

    // --- Seeking (before "vuelve" and "adelante") -------------------------
    {K::kRestart,
     "(reinicia|reiniciar|empieza de nuevo|empieza otra vez|vuelve a "
     "empezar|desde el principio|desde el inicio|ponlo otra vez|ponlo de "
     "nuevo|reproduce otra vez|reproduce de nuevo|reinicia el video|"
     "rebobina hasta el principio|rebobina al principio)"},
    {K::kSkipBack,
     "(retrocede|retroceder|retroceda|atrasa|atrasar|vuelve|regresa|salta "
     "hacia atras|ve hacia atras) {seconds}"},
    {K::kSkipBack, "(rebobina|rebobinar|rebobine) {seconds?}"},
    {K::kSkipBack, "atras {seconds}"},
    {K::kSkipForward,
     "(adelanta|adelantar|adelante|avanza|avanzar|avance|salta|saltar|"
     "salta hacia adelante|ve hacia adelante) {seconds}"},
    {K::kSkipForward,
     "(adelanta|adelantar|adelantalo|salta hacia adelante|avance rapido) "
     "{seconds?}"},
    {K::kSkipForward, "(salta|saltar|saltate) {seconds?}"},

    // --- Going places -------------------------------------------------------
    {K::kHome,
     "(ve a inicio|ir a inicio|inicio|pagina de inicio|pagina "
     "principal|llevame a inicio|ve a la pagina de inicio|ir a la pagina de "
     "inicio|abre la pagina de inicio|ve a casa)"},
    {K::kBack,
     "(ve|ir|vuelve|volver|regresa|regresar|llevame) (atras|hacia atras|a "
     "la pagina anterior)"},
    {K::kBack,
     "(atras|vuelve|volver|regresa|regresar|pagina anterior|retrocede|"
     "retroceder)"},
    {K::kForward, "(ve|ir|avanza|avanzar) (adelante|hacia adelante)"},
    {K::kForward, "(adelante|avanza|avanzar)"},
    {K::kNextPage,
     "[ve a|ir a|pasa a] [la] (pagina siguiente|siguiente pagina|proxima "
     "pagina) [de resultados]"},
    {K::kNextPage,
     "(mas resultados|muestra mas resultados|muestrame mas resultados|carga "
     "mas resultados|ver mas resultados)"},
    {K::kReload,
     "(recarga|recargar|recargue|actualiza|actualizar|actualice|refresca|"
     "refrescar) [la|esta] [pagina|pestana|web]"},
    {K::kReload,
     "(vuelve a cargar|carga de nuevo|carga otra vez) [la|esta] [pagina]"},
    {K::kNewTab,
     "(abre|abrir|abra|crea|crear) [una] (nueva pestana|pestana nueva|"
     "pestana|otra pestana)"},
    {K::kNewTab, "(nueva pestana|pestana nueva)"},
    {K::kCloseTab,
     "(cierra|cerrar|cierre) (esta pestana|la pestana|la pestana actual|"
     "pestana)"},
    {K::kCloseTab, "(cierra|cerrar|cierre) [la] pestana {tab}"},
    {K::kCloseTab, "(cierra|cerrar|cierre) [la] {tab} pestana"},
    {K::kSwitchTab,
     "[ve a|cambia a|pasa a|ir a] [la] (siguiente pestana|pestana "
     "siguiente|proxima pestana)",
     C::kAlways, V::kNextTab},
    {K::kSwitchTab,
     "[ve a|cambia a|pasa a|ir a|vuelve a] [la] (pestana anterior|anterior "
     "pestana)",
     C::kAlways, V::kPreviousTab},
    {K::kSwitchTab, "(cambia|cambiar|ve|ir|pasa|salta) a la pestana {tab}"},
    {K::kSwitchTab, "pestana {tab}"},
    {K::kSwitchTab, "(cambia|cambiar|ve|ir|pasa|salta) a la {tab} pestana"},
    {K::kSwitchTab, "(cambia|cambiar) a {tab}"},

    // --- Viewing ------------------------------------------------------------
    {K::kZoomReset,
     "((restablece|restablecer|reinicia|quita) [el] zoom|(zoom|tamano) "
     "(normal|original|por defecto|real))"},
    {K::kZoomIn,
     "((acerca|acercar|amplia|ampliar|agranda|agrandar) [el zoom|la pagina|"
     "el texto|todo]|(aumenta|aumentar) el zoom|mas grande|texto mas "
     "grande|letra mas grande|mas zoom|haz zoom|hazlo mas grande)"},
    {K::kZoomOut,
     "((aleja|alejar|achica|achicar|encoge|encoger) [el zoom|la pagina|el "
     "texto|todo]|(reduce|reducir|disminuye|disminuir) el zoom|mas pequeno|"
     "texto mas pequeno|letra mas pequena|menos zoom|hazlo mas pequeno)"},

    // --- Media --------------------------------------------------------------
    {K::kPause,
     "(pausa|pausar|pausalo|pausala|pon pausa|espera|un momento) [el "
     "video|la musica|la cancion|la reproduccion|la pelicula|el audio|la "
     "lectura|esto|eso]"},
    {K::kSkipAd,
     "(salta|saltar|salte|omite|omitir|omita|quita|saltate) [el|este|los|"
     "estos] (anuncio|anuncios|publicidad|comercial)"},
    {K::kResume,
     "(reanuda|reanudar|continua|continuar|sigue|seguir|quita la pausa|"
     "sigue reproduciendo|continua reproduciendo) [el video|la musica|la "
     "cancion|la reproduccion]"},
    {K::kNextTrack,
     "[pon|reproduce|salta a|pasa a|ve a] (el|la) (siguiente|proximo|"
     "proxima) (video|cancion|tema|pista|episodio|clip)"},
    {K::kNextTrack,
     "[pon|reproduce|salta a|pasa a|ve a] (el|la) (video|cancion|tema|"
     "pista|episodio|clip) siguiente"},
    {K::kNextTrack,
     "(siguiente|proximo|proxima) (video|cancion|tema|pista|episodio|clip)"},
    {K::kNextTrack,
     "(salta|saltar|pasa|pasar|omite) (este|esta|el|la) (video|cancion|"
     "tema|pista|episodio|clip)"},
    {K::kPreviousTrack,
     "[pon|reproduce|vuelve a|ve a] (el|la) (video|cancion|tema|pista|"
     "episodio|clip) anterior"},
    {K::kPreviousTrack,
     "[pon|reproduce|vuelve a|ve a] (el|la) anterior (video|cancion|tema|"
     "pista|episodio|clip)"},
    {K::kPreviousTrack, "(video|cancion|tema|pista|episodio|clip) anterior"},
    {K::kFullScreen,
     "[pon|poner|ponlo en|ponla en|activa|activar|ve a|entra en|entrar en|"
     "cambia a|modo] [la] pantalla completa"},
    {K::kFullScreen, "(maximiza|maximizar) [el video|el reproductor]"},
    {K::kExitFullScreen,
     "(sal de|salir de|quita|quitar|cierra|cerrar|desactiva|desactivar) "
     "[la] pantalla completa"},
    {K::kExitFullScreen,
     "(pantalla normal|minimiza|minimizar) [el video|el reproductor]"},
    {K::kCaptionsOn,
     "(activa|activar|pon|poner|muestra|mostrar|enciende|encender) [los] "
     "subtitulos"},
    {K::kCaptionsOn, "subtitulos (activados|si|on)"},
    {K::kCaptionsOff,
     "(desactiva|desactivar|quita|quitar|oculta|ocultar|apaga|apagar) "
     "[los] subtitulos"},
    {K::kCaptionsOff, "(subtitulos (desactivados|no|off)|sin subtitulos)"},
    {K::kNormalSpeed,
     "([pon|reproduce a] [la] velocidad normal|velocidad por defecto|"
     "restablece la velocidad)"},
    {K::kSpeedUp,
     "(mas rapido|acelera|acelerar|aceleralo|reproduce mas rapido|aumenta "
     "la velocidad|sube la velocidad)"},
    {K::kSlowDown,
     "(mas lento|mas despacio|ralentiza|reproduce mas lento|reduce la "
     "velocidad|baja la velocidad)"},
    {K::kVolumeUp,
     "(mas alto|sube el volumen|subele|sube el sonido|subelo|sube volumen|"
     "volumen arriba|mas volumen|aumenta el volumen|un poco mas alto|mucho "
     "mas alto|mas fuerte|sube un poco el volumen)"},
    {K::kVolumeDown,
     "(mas bajo|baja el volumen|bajale|baja el sonido|bajalo|baja volumen|"
     "volumen abajo|menos volumen|reduce el volumen|disminuye el volumen|un "
     "poco mas bajo|mucho mas bajo|mas flojo|mas suave|baja un poco el "
     "volumen)"},
    {K::kSetVolume,
     "[pon|poner|ajusta|ajustar|cambia|deja] [el] volumen [a|al|en] "
     "{level}"},
    {K::kMute,
     "(silencia|silenciar|silencie|mutea|mutear) [esto|todo|el (sonido|"
     "audio|video|volumen)|la (pestana|musica)]"},
    {K::kMute,
     "((quita|quitar|apaga|apagar) el (sonido|audio)|sin sonido|sonido "
     "(off|desactivado|apagado))"},
    {K::kUnmute,
     "((activa|activar|reactiva|pon|enciende|devuelve) el (sonido|audio)|"
     "(quita|quitar) el silencio|desmutea|desmutear|sonido (on|activado|"
     "encendido))"},
    {K::kPlay,
     "(reproduce|reproducir|reproduzca|play|dale al play|pon|poner|ponlo|"
     "ponla|reproducelo|reproducela|empieza a reproducir) [esto|eso|el "
     "video|la cancion|la musica|la pelicula]"},

    // --- Reading ------------------------------------------------------------
    {K::kReadSelection,
     "(lee|leeme|leer) [la|lo] (seleccion|texto seleccionado|texto "
     "resaltado|seleccionado|resaltado)"},
    {K::kReadSelection,
     "(lee|leeme|leer) lo que (esta|he) (seleccionado|resaltado)"},
    {K::kReadTitle,
     "(lee|leeme|dime|di) el titulo [de la pagina|de esta pagina]"},
    {K::kReadTitle,
     "(cual es el titulo [de la pagina|de esta pagina]|como se llama (la|"
     "esta) pagina)"},
    {K::kReadHeadings,
     "(lee|leeme|lista|di|dime|cuales son) [todos] (los titulos|los "
     "encabezados|las secciones|los apartados)"},
    {K::kReadHeadings, "(titulos|encabezados)"},
    {K::kReadLinks,
     "(lee|leeme|lista|di|dime|cuales son) [todos] los (enlaces|vinculos|"
     "links)"},
    {K::kReadResults,
     "(lee|leeme|lista|di|dime|cuales son) [todos|todas] (los resultados|"
     "las opciones|los elementos|la lista|los videos|las noticias|los "
     "articulos|los productos|los resultados de busqueda) [otra vez|de "
     "nuevo]"},
    {K::kReadResults, "cuales son mis opciones"},
    {K::kReadFromHere,
     "(lee desde aqui|leer desde aqui|sigue leyendo|continua leyendo|lee el "
     "resto [de la pagina]|lee lo que queda|lee a partir de aqui|reanuda la "
     "lectura|retoma la lectura)"},
    {K::kReadPage,
     "(lee|leeme|leer|lea) (la|esta|toda la) (pagina|noticia|publicacion) "
     "[completa|entera] [en voz alta]"},
    {K::kReadPage,
     "(lee|leeme|leer|lea) (el|este|todo el) (articulo|texto|contenido) "
     "[completo|entero] [en voz alta]"},
    {K::kReadPage,
     "(lee|leeme|leer|empieza a leer|comienza a leer|lee todo|leelo|leelo "
     "todo|leemelo) [en voz alta]"},
    {K::kDescribeScreen,
     "(que hay en (la|mi|esta) (pantalla|pagina)|que ves|que puedes ver|"
     "describe (la|esta) (pantalla|pagina|vista)|describeme la pantalla|"
     "que hay aqui)"},
    {K::kNextHeading,
     "[ve al|ve a la|salta al|salta a la|pasa al|pasa a la|lee el|lee la] "
     "(siguiente|proximo|proxima) (titulo|encabezado|seccion|apartado)"},
    {K::kNextHeading,
     "[ve al|ve a la|salta al|salta a la|pasa al|pasa a la|lee el|lee la] "
     "(titulo|encabezado|seccion|apartado) siguiente"},
    {K::kPreviousHeading,
     "[ve al|ve a la|salta al|salta a la|pasa al|pasa a la|lee el|lee la|"
     "vuelve al|vuelve a la] (titulo|encabezado|seccion|apartado) anterior"},
    {K::kPreviousHeading,
     "[ve al|ve a la|salta al|salta a la|pasa al|pasa a la|lee el|lee la|"
     "vuelve al|vuelve a la] anterior (titulo|encabezado|seccion|apartado)"},
    {K::kNextLink,
     "[ve al|salta al|pasa al|lee el] (siguiente|proximo) (enlace|vinculo|"
     "link)"},
    {K::kNextLink,
     "[ve al|salta al|pasa al|lee el] (enlace|vinculo|link) siguiente"},
    {K::kPreviousLink,
     "[ve al|salta al|pasa al|lee el|vuelve al] (enlace|vinculo|link) "
     "anterior"},
    {K::kPreviousLink,
     "[ve al|salta al|pasa al|lee el|vuelve al] anterior (enlace|vinculo|"
     "link)"},
    {K::kNextParagraph,
     "[ve al|salta al|pasa al|lee el] (siguiente|proximo) parrafo"},
    {K::kNextParagraph, "[ve al|salta al|pasa al|lee el] parrafo siguiente"},
    {K::kPreviousParagraph,
     "[ve al|salta al|pasa al|lee el|vuelve al] parrafo anterior"},
    {K::kPreviousParagraph,
     "[ve al|salta al|pasa al|lee el|vuelve al] anterior parrafo"},
    {K::kNextItem,
     "[ve al|ve a la|salta al|salta a la|pasa al|pasa a la|lee el|lee la] "
     "(siguiente|proximo|proxima) (elemento|resultado|entrada|opcion)"},
    {K::kNextItem,
     "[ve al|ve a la|salta al|salta a la|pasa al|pasa a la|lee el|lee la] "
     "(elemento|resultado|entrada|opcion) siguiente"},
    {K::kNextItem, "siguiente"},
    {K::kPreviousItem,
     "[ve al|ve a la|salta al|salta a la|pasa al|pasa a la|lee el|lee la|"
     "vuelve al|vuelve a la] (elemento|resultado|entrada|opcion) anterior"},
    {K::kPreviousItem, "anterior"},
    {K::kReadSection,
     "(lee|leeme|ve a|salta a|pasa a|ir a) la (seccion|parte) (sobre|de|"
     "que habla de|llamada|titulada) {query}"},
    {K::kReadSection,
     "(lee|leeme|ve a|salta a|pasa a|ir a) el apartado (sobre|de|que habla "
     "de|llamado|titulado) {query}"},

    // --- Questions about the page (page_questions.h) ----------------------
    // Before choosing, which would take "que es <a description>". The
    // questions that are commands ("que hay en la pantalla", "donde estoy",
    // "cual es el titulo") are above and stay commands; a question none of
    // these takes still goes to the planner.
    {K::kAskPage,
     "(cuentame mas|dime mas|que mas dice [la pagina|el articulo|el "
     "texto]|sigue contando)",
     C::kAlways, V::kAskMore},
    {K::kAskPage,
     "(lee|leeme|dime|dame|di|resume) (los puntos|las ideas|los aspectos) "
     "(principales|clave|importantes|mas importantes)",
     C::kAlways, V::kAskMainPoints},
    {K::kAskPage,
     "cuales son (los puntos|las ideas|los aspectos) (principales|clave|"
     "importantes|mas importantes)",
     C::kAlways, V::kAskMainPoints},
    {K::kAskPage,
     "(puntos principales|puntos clave|ideas principales|ideas clave)",
     C::kAlways, V::kAskMainPoints},
    {K::kAskPage,
     "que dice [la pagina|esta pagina|el articulo|este articulo|la noticia|"
     "esta noticia|el texto|el sitio|la web] (sobre|de|acerca de|respecto "
     "a|respecto de) {query}",
     C::kAlways, V::kAskTopic},
    {K::kAskPage,
     "(dice|menciona|cuenta) (algo|alguna cosa) (sobre|de|acerca de) "
     "{query}",
     C::kAlways, V::kAskTopic},
    {K::kAskPage,
     "(menciona|habla de|habla sobre|trata sobre|trata de) {query}", C::kAlways,
     V::kAskTopic},
    {K::kAskPage,
     "(dime|leeme) que dice (la pagina|esta pagina|el articulo|este "
     "articulo|el texto) (sobre|de|acerca de) {query}",
     C::kAlways, V::kAskTopic},
    {K::kAskPage,
     "de que (trata|va|habla) [esta pagina|la pagina|este articulo|el "
     "articulo|esto|este video|el video|esta web|el texto|este texto|la "
     "noticia|esta noticia]",
     C::kAlways, V::kAskAbout},
    {K::kAskPage,
     "que dice (la pagina|esta pagina|el articulo|este articulo|el texto|"
     "la noticia|esta noticia)",
     C::kAlways, V::kAskAbout},
    {K::kAskPage,
     "(resume|resumir|resumeme|haz un resumen de|hazme un resumen de) "
     "[esto|eso|la pagina|esta pagina|el articulo|este articulo|el texto|el "
     "video|este video]",
     C::kAlways, V::kAskAbout},
    {K::kAskPage,
     "(dame|dime|leeme|hazme|haz) un resumen [de (esto|eso|la pagina|esta "
     "pagina|el articulo|este articulo|el texto)]",
     C::kAlways, V::kAskAbout},
    {K::kAskPage, "(resumen|un resumen)", C::kAlways, V::kAskAbout},
    {K::kAskPage,
     "quien (escribio|ha escrito|publico|firma) (esto|este articulo|el "
     "articulo|la pagina|esta pagina|la noticia|esta noticia)",
     C::kAlways, V::kAskQuestion},
    {K::kAskPage,
     "quien es el (autor|escritor|editor) [de (esto|este articulo|el "
     "articulo|la noticia|esta noticia)]",
     C::kAlways, V::kAskQuestion},
    {K::kAskPage,
     "(cuanto cuesta|cuanto vale|que precio tiene|cual es el precio [de "
     "(esto|este producto|el producto)]) [esto|este|esta|eso|el producto|"
     "este producto]",
     C::kAlways, V::kAskQuestion},
    {K::kAskPage,
     "cuando (se publico|se escribio|se actualizo|fue publicado|fue "
     "escrito|fue actualizado) (esto|este articulo|el articulo|la pagina|"
     "esta pagina|la noticia|esta noticia)",
     C::kAlways, V::kAskQuestion},
    {K::kAskPage,
     "(preguntale a la pagina|pregunta a la pagina|pregunta al articulo|"
     "preguntale al articulo) {query}",
     C::kAlways, V::kAskQuestion},
    {K::kAskPage,
     "segun (la pagina|esta pagina|el articulo|este articulo|la web|la "
     "noticia) {query}",
     C::kAlways, V::kAskQuestion},

    // --- Choosing -----------------------------------------------------------
    {K::kShowNumbers,
     "((muestra|muestrame|mostrar|ensena|pon|activa) [los] (numeros|"
     "etiquetas)|(numeros|etiquetas) (activados|on)|numera todo)"},
    {K::kHideNumbers,
     "((oculta|ocultar|esconde|quita|quitar|desactiva) [los] (numeros|"
     "etiquetas)|(numeros|etiquetas) (off|desactivados)|sin numeros)"},
    // A choice by number or position wins over a site of the same words.
    {K::kChoose,
     "(reproduce|reproducir|pon|poner|ponme|mira|ver|escucha|escuchar) "
     "{choice}",
     C::kAlways, V::kVerbPlay},
    {K::kChoose,
     "(abre|abrir|abra|ve a|ir a|entra en|sigue|visita|muestra|muestrame|"
     "ensename) {position}",
     C::kAlways, V::kVerbOpen},
    {K::kChoose,
     "(haz clic en|haz click en|hacer clic en|clic en|click en|pulsa|"
     "pulsar|pulse|presiona|presionar|toca|tocar|aprieta|marca|desmarca) "
     "{choice}",
     C::kAlways, V::kVerbClick},
    {K::kChoose,
     "(selecciona|seleccionar|seleccione|elige|elegir|escoge|escoger|"
     "enfoca) {choice}",
     C::kAlways, V::kVerbSelect},
    {K::kChoose, "(lee|leeme) {choice}", C::kAlways, V::kVerbRead},
    {K::kChoose,
     "(describe|describeme|que es|hablame de|informacion sobre|detalles "
     "de|mas sobre) {choice}",
     C::kAlways, V::kVerbDescribe},
    // Right after a list, a description beats a site name.
    {K::kChoose,
     "(abre|abrir|abra|ve a|ir a|entra en|visita|muestra|muestrame) "
     "{choice}",
     C::kChoice, V::kVerbOpen},

    // --- Sites and searches -------------------------------------------------
    {K::kOpenSite,
     "(abre|abrir|abra|ve a|ir a|vaya a|llevame a|visita|navega a|carga|"
     "lanza|entra en|entrar en|entra a) {site} en [una] (nueva pestana|"
     "pestana nueva|otra pestana)",
     C::kAlways, V::kNewTab},
    {K::kOpenSite,
     "(abre|abrir|abra|ve a|ir a|vaya a|llevame a|visita|navega a|carga|"
     "lanza|entra en|entrar en|entra a) {site}"},
    {K::kChoose,
     "(abre|abrir|abra|ve a|ir a|entra en|visita|muestra|muestrame) "
     "{choice}",
     C::kAlways, V::kVerbOpen},
    {K::kFindInPage,
     "(busca|buscar|encuentra|encontrar) [la palabra] {query} en (esta|la) "
     "pagina"},
    {K::kFindInPage, "(busca|buscar|encuentra) en (esta|la) pagina {query}"},
    {K::kSearch, "(busca|buscar|busque) en (internet|la web|la red) {query}",
     C::kAlways, V::kScopeDefaultEngine},
    {K::kSearch, "(busca|buscar|busque) {query} en (internet|la web|la red)",
     C::kAlways, V::kScopeDefaultEngine},
    {K::kSearch,
     "(busca|buscar|busque) en (este sitio|esta web|esta pagina web|aqui) "
     "{query}",
     C::kAlways, V::kScopeCurrentSite},
    {K::kSearch,
     "(busca|buscar|busque) {query} (en este sitio|en esta web|aqui)",
     C::kAlways, V::kScopeCurrentSite},
    {K::kSearch, "en {site} (busca|buscar|busque) {query}", C::kAlways,
     V::kScopeNamedSite},
    {K::kSearch, "(busca|buscar|busque|buscame) {query} en {site}", C::kAlways,
     V::kScopeNamedSite},
    {K::kSearch, "(busca|buscar|busque|buscame|encuentra|encuentrame) {query}"},
    // An address said on its own: "ejemplo punto com".
    {K::kOpenSite, "{address}"},

    // --- A choice on its own ------------------------------------------------
    {K::kChoose, "{number}", C::kChoice},
    {K::kChoose, "{position}"},
    {K::kChoose, "{pick}", C::kChoice},
};

constexpr CommandInfo kInfos[] = {
    // Going places.
    {K::kOpenSite, "Ir a sitios", "Abre un sitio por su nombre o su dirección.",
     "abre YouTube"},
    {K::kSearch, "Ir a sitios",
     "Busca: en el sitio en el que estás (o en el buscador si no tiene "
     "cuadro de búsqueda), en la web, o en el sitio que digas.",
     "busca Bohemian Rhapsody"},
    {K::kFindInPage, "Ir a sitios", "Encuentra palabras en esta página.",
     "busca horario en esta página"},
    {K::kBack, "Ir a sitios", "Vuelve a la página anterior.", "atrás"},
    {K::kForward, "Ir a sitios", "Avanza otra vez.", "adelante"},
    {K::kReload, "Ir a sitios", "Vuelve a cargar la página.", "recarga"},
    {K::kStopLoading, "Ir a sitios", "Deja de cargar la página.",
     "deja de cargar"},
    {K::kHome, "Ir a sitios", "Ve a tu página de inicio.", "ve a inicio"},
    {K::kNextPage, "Ir a sitios", "La página siguiente de resultados.",
     "página siguiente"},
    {K::kNewTab, "Ir a sitios", "Abre una pestaña nueva.",
     "abre una pestaña nueva"},
    {K::kCloseTab, "Ir a sitios",
     "Cierra esta pestaña, o una pestaña por su número o su nombre.",
     "cierra esta pestaña"},
    {K::kSwitchTab, "Ir a sitios",
     "Cambia de pestaña: por número, posición o nombre, o la siguiente.",
     "cambia a la pestaña 2"},
    // Viewing.
    {K::kScrollDown, "Ver", "Baja: un poco, media página, una página o varias.",
     "baja"},
    {K::kScrollUp, "Ver", "Sube.", "sube una página"},
    {K::kScrollLeft, "Ver", "Desplázate a la izquierda.",
     "desplázate a la izquierda"},
    {K::kScrollRight, "Ver", "Desplázate a la derecha.",
     "desplázate a la derecha"},
    {K::kScrollToTop, "Ver", "Ve al principio de la página.",
     "ve al principio"},
    {K::kScrollToBottom, "Ver", "Ve al final de la página.", "ve al final"},
    {K::kZoomIn, "Ver", "Hazlo todo más grande.", "acerca"},
    {K::kZoomOut, "Ver", "Hazlo todo más pequeño.", "aleja"},
    {K::kZoomReset, "Ver", "Vuelve al tamaño normal.", "restablece el zoom"},
    // Media.
    {K::kPlay, "Multimedia", "Reproduce.", "reproduce"},
    {K::kPause, "Multimedia", "Pausa.", "pausa"},
    {K::kResume, "Multimedia", "Sigue reproduciendo.", "reanuda"},
    {K::kVolumeUp, "Multimedia", "Más alto.", "sube el volumen"},
    {K::kVolumeDown, "Multimedia", "Más bajo.", "baja el volumen"},
    {K::kSetVolume, "Multimedia",
     "Pon el volumen: de 0 a 10, un porcentaje, la mitad o el máximo.",
     "pon el volumen a 5"},
    {K::kMute, "Multimedia", "Quita el sonido.", "silencia"},
    {K::kUnmute, "Multimedia", "Devuelve el sonido.", "activa el sonido"},
    {K::kSkipForward, "Multimedia", "Adelanta, un paso o lo que digas.",
     "adelanta un minuto"},
    {K::kSkipBack, "Multimedia", "Retrocede en el vídeo.",
     "retrocede 10 segundos"},
    {K::kRestart, "Multimedia", "Reproduce desde el principio.",
     "empieza de nuevo"},
    {K::kNextTrack, "Multimedia", "El vídeo o la canción siguiente.",
     "siguiente vídeo"},
    {K::kPreviousTrack, "Multimedia", "El vídeo o la canción anterior.",
     "canción anterior"},
    {K::kFullScreen, "Multimedia", "Pantalla completa.", "pantalla completa"},
    {K::kExitFullScreen, "Multimedia", "Sal de la pantalla completa.",
     "sal de la pantalla completa"},
    {K::kCaptionsOn, "Multimedia", "Muestra los subtítulos.",
     "activa los subtítulos"},
    {K::kCaptionsOff, "Multimedia", "Oculta los subtítulos.",
     "quita los subtítulos"},
    {K::kSpeedUp, "Multimedia", "Reproduce más rápido.", "acelera"},
    {K::kSlowDown, "Multimedia", "Reproduce más lento.", "más lento"},
    {K::kNormalSpeed, "Multimedia", "Vuelve a la velocidad normal.",
     "velocidad normal"},
    {K::kSkipAd, "Multimedia",
     "Salta el anuncio, cuando el reproductor lo permita.", "salta el anuncio"},
    // Reading.
    {K::kReadPage, "Lectura", "Lee la página en voz alta.", "lee la página"},
    {K::kReadFromHere, "Lectura", "Sigue leyendo desde donde estás.",
     "lee desde aquí"},
    {K::kReadHeadings, "Lectura", "Lee los títulos.", "lee los títulos"},
    {K::kReadLinks, "Lectura", "Lee los enlaces.", "lee los enlaces"},
    {K::kReadResults, "Lectura", "Vuelve a leer la lista de resultados.",
     "lee los resultados"},
    {K::kReadTitle, "Lectura", "Lee el título de la página.", "lee el título"},
    {K::kReadSelection, "Lectura", "Lee el texto seleccionado.",
     "lee la selección"},
    {K::kDescribeScreen, "Lectura", "Describe lo que hay en la pantalla.",
     "qué hay en la pantalla"},
    {K::kNextHeading, "Lectura", "Pasa al título siguiente.",
     "siguiente título"},
    {K::kPreviousHeading, "Lectura", "Vuelve al título anterior.",
     "título anterior"},
    {K::kNextLink, "Lectura", "Pasa al enlace siguiente.", "siguiente enlace"},
    {K::kPreviousLink, "Lectura", "Vuelve al enlace anterior.",
     "enlace anterior"},
    {K::kNextItem, "Lectura", "Pasa al elemento siguiente de la lista.",
     "siguiente elemento"},
    {K::kPreviousItem, "Lectura", "Vuelve al elemento anterior.",
     "elemento anterior"},
    {K::kNextParagraph, "Lectura", "Lee el párrafo siguiente.",
     "siguiente párrafo"},
    {K::kPreviousParagraph, "Lectura", "Lee el párrafo anterior.",
     "párrafo anterior"},
    {K::kReadSection, "Lectura", "Lee una sección de la página, por su título.",
     "lee la sección sobre historia"},
    // Questions about the page.
    {K::kAskPage, "Preguntas sobre la página",
     "Pregunta por la página en la que estás: de qué trata, sus puntos "
     "principales, qué dice sobre algo, quién la escribió, el precio. La IA "
     "del dispositivo responde a partir del texto de la página, brevemente, "
     "y dice cuando la página no lo dice. \"Cuéntame más\" sigue con los "
     "pasajes siguientes.",
     "de qué trata esta página"},
    // Choosing.
    {K::kChoose, "Elegir",
     "Elige de lo que hay en la pantalla: por número, por posición o por "
     "nombre, con lo que quieres hacer (abrir, reproducir, pulsar, "
     "seleccionar, leer, describir).",
     "reproduce el cuarto"},
    {K::kShowNumbers, "Elegir", "Pon números en todo lo que se puede elegir.",
     "muestra los números"},
    {K::kHideNumbers, "Elegir", "Quita los números.", "oculta los números"},
    // Typing.
    {K::kType, "Escribir", "Escribe en el campo seleccionado.",
     "escribe hola mundo"},
    {K::kNewLine, "Escribir", "Empieza una línea nueva.", "nueva línea",
     C::kTextField},
    {K::kDeleteLastWord, "Escribir", "Borra la última palabra.",
     "borra la última palabra", C::kTextField},
    {K::kClearField, "Escribir", "Vacía el campo.", "borra el campo",
     C::kTextField},
    {K::kReadBack, "Escribir", "Lee lo que has escrito.",
     "lee lo que he escrito", C::kTextField},
    {K::kSubmit, "Escribir", "Envía lo que has escrito.", "envía",
     C::kTextField},
    // Conversation.
    {K::kYes, "Hablar con el navegador", "Responde a una pregunta: sí.", "sí",
     C::kYesNo},
    {K::kNo, "Hablar con el navegador", "Responde a una pregunta: no.", "no",
     C::kYesNo},
    {K::kCancel, "Hablar con el navegador", "Cancela lo que pediste.",
     "cancela"},
    {K::kStop, "Hablar con el navegador",
     "Para: de hablar, y lo que esté en marcha.", "para"},
    {K::kStopSpeaking, "Hablar con el navegador", "Deja de hablar.",
     "deja de hablar"},
    {K::kUndo, "Hablar con el navegador", "Deshaz la última acción.", "deshaz"},
    {K::kRepeat, "Hablar con el navegador", "Dilo otra vez.", "repite"},
    {K::kHelp, "Hablar con el navegador", "Escucha lo que puedes decir.",
     "qué puedo decir"},
    {K::kWhereAmI, "Hablar con el navegador", "Escucha dónde estás.",
     "dónde estoy"},
    {K::kSpeakFaster, "Hablar con el navegador", "Habla más rápido.",
     "habla más rápido"},
    {K::kSpeakSlower, "Hablar con el navegador", "Habla más despacio.",
     "habla más despacio"},
    {K::kVerbosity, "Hablar con el navegador",
     "Cuánto dice el navegador: breve, normal o completo, más o menos.",
     "sé breve"},
    {K::kCorrection, "Hablar con el navegador", "Corrige lo que se oyó.",
     "no, dije YouTube"},
    // Routines.
    {K::kRememberRoutine, "Rutinas",
     "Guarda lo que acabas de hacer como una rutina.",
     "recuerda esto como mis noticias"},
    {K::kRunRoutine, "Rutinas", "Ejecuta una rutina por su nombre.",
     "ejecuta mis noticias"},
    {K::kForgetRoutine, "Rutinas", "Borra una rutina.", "olvida mis noticias"},
    {K::kListRoutines, "Rutinas", "Escucha tus rutinas.",
     "cuáles son mis rutinas"},
    {K::kForgetWord, "Rutinas",
     "Olvida una palabra o una corrección que el navegador aprendió de ti.",
     "olvida la palabra Queen"},
    {K::kForgetLearning, "Rutinas",
     "Olvida todo lo que el navegador aprendió de ti: rutinas, palabras, "
     "correcciones y pistas de sitios.",
     "olvida todo lo que aprendiste"},
    // Voice mode itself.
    {K::kVoiceModeOn, "Modo de voz",
     "Empieza a escuchar (escrito en el panel del modo Agente).",
     "activa el modo de voz"},
    {K::kVoiceModeOff, "Modo de voz",
     "Deja de escuchar. El navegador pregunta antes: después, el botón Voz "
     "de la barra de herramientas, o la configuración, lo vuelven a "
     "activar.",
     "desactiva el modo de voz"},
};

}  // namespace

std::vector<CommandPattern> MakeSpanishCommandPatterns() {
  return std::vector<CommandPattern>(std::begin(kPatterns),
                                     std::end(kPatterns));
}

std::vector<CommandInfo> MakeSpanishCommandInfos() {
  return std::vector<CommandInfo>(std::begin(kInfos), std::end(kInfos));
}

}  // namespace voice
}  // namespace molt_ai
