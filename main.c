#include <gtk/gtk.h>
#include <iec61850_client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// 1. DEFINIÇÃO DO OBJETO DA ÁRVORE (GObject customizado)
// ============================================================================
typedef enum {
    NODE_LD,
    NODE_LN,
    NODE_DO,
    NODE_DA
} IedNodeType;

#define IED_TYPE_NODE (ied_node_get_type())
G_DECLARE_FINAL_TYPE(IedNode, ied_node, IED, NODE, GObject)

struct _IedNode {
    GObject parent_instance;
    char *name;
    char *reference;
    IedNodeType type;
    FunctionalConstraint fc;
};

G_DEFINE_TYPE(IedNode, ied_node, G_TYPE_OBJECT)

static void ied_node_finalize(GObject *object) {
    IedNode *self = IED_NODE(object);
    g_free(self->name);
    g_free(self->reference);
    G_OBJECT_CLASS(ied_node_parent_class)->finalize(object);
}

static void ied_node_class_init(IedNodeClass *klass) {
    G_OBJECT_CLASS(klass)->finalize = ied_node_finalize;
}

static void ied_node_init(IedNode *self) {
    (void)self;
}

static IedNode *ied_node_new(const char *name, const char *reference, IedNodeType type, FunctionalConstraint fc) {
    IedNode *node = g_object_new(IED_TYPE_NODE, NULL);
    node->name = g_strdup(name);
    node->reference = g_strdup(reference);
    node->type = type;
    node->fc = fc;
    return node;
}

// ============================================================================
// 2. ESTRUTURA GLOBAL DA APLICAÇÃO
// ============================================================================
typedef struct {
    GtkWidget *window;
    GtkWidget *entry_ip;
    GtkWidget *entry_port;
    GtkWidget *btn_connect;
    GtkWidget *btn_disconnect;
    GtkWidget *lbl_status;

    // Árvore
    GListStore *root_store;
    GtkTreeListModel *tree_model;
    GtkSingleSelection *selection_model;

    // Painel de Detalhes e Leitura
    GtkWidget *lbl_ref;
    GtkWidget *lbl_type;
    GtkWidget *lbl_fc;
    GtkWidget *lbl_value;
    GtkWidget *btn_refresh;

    // Painel de Escrita / Comando
    GtkWidget *entry_write;
    GtkWidget *btn_write;
    MmsType current_mms_type;
    int current_bitstring_size;
    gboolean is_control_mode;

    // Controles de Auto-Atualização (Polling)
    GtkWidget *spin_interval;
    GtkWidget *btn_auto_refresh;
    guint auto_refresh_timer_id;
    gboolean is_reading;

    // Estado da Conexão
    IedConnection con;
    gboolean is_connected;
    GMutex net_mutex;
    IedNode *selected_node;
} AppState;

typedef struct {
    AppState *state;
    char *ip;
    int port;
    IedConnection con;
    IedClientError error;
    LinkedList device_list;
} ConnectTaskData;

typedef struct {
    AppState *state;
    char *reference;
    FunctionalConstraint fc;
    char *result_text;
    MmsType mms_type;
    int bitstring_size;
    gboolean read_ok;
} ReadTaskData;

typedef struct {
    AppState *state;
    char *reference;
    FunctionalConstraint fc;
    char *input_text;
    MmsType mms_type;
    int bitstring_size;
    gboolean is_control;
    IedClientError error;
    gboolean parse_ok;
    gboolean control_success;
    char *detail_msg;
} WriteTaskData;

// Protótipo antecipado
static void trigger_async_read(AppState *state);

// ============================================================================
// 3. AUXILIARES DE PERMISSÃO, CONTROLE E CONVERSÃO MMS <-> TEXTO
// ============================================================================
static gboolean is_fc_writable(FunctionalConstraint fc) {
    switch (fc) {
        case IEC61850_FC_SP: // Setpoint
        case IEC61850_FC_CF: // Configuration
        case IEC61850_FC_DC: // Description
        case IEC61850_FC_SV: // Substitution
        case IEC61850_FC_SE: // Setting group editable
        case IEC61850_FC_SG: // Setting group
            return TRUE;
        default:
            return FALSE;
    }
}

static gboolean is_mms_type_editable(MmsType type) {
    switch (type) {
        case MMS_BOOLEAN:
        case MMS_INTEGER:
        case MMS_UNSIGNED:
        case MMS_FLOAT:
        case MMS_VISIBLE_STRING:
        case MMS_STRING:
        case MMS_BIT_STRING:
            return TRUE;
        default:
            return FALSE;
    }
}

/* Extrai a referência base do Data Object de controle (ex: "GenericIO/GGIO1.SPCSO1.Oper" -> "GenericIO/GGIO1.SPCSO1") */
static char *extract_control_object_ref(const char *ref) {
    if (!ref) return NULL;
    char *copy = g_strdup(ref);
    const char *suffixes[] = {".Oper.ctlVal", ".SBOw.ctlVal", ".Oper", ".SBOw", ".SBO", ".Cancel"};
    for (size_t i = 0; i < G_N_ELEMENTS(suffixes); i++) {
        if (g_str_has_suffix(copy, suffixes[i])) {
            copy[strlen(copy) - strlen(suffixes[i])] = '\0';
            break;
        }
    }
    return copy;
}

static char *mms_value_to_string(MmsValue *val) {
    if (!val) return g_strdup("NULL");

    MmsType type = MmsValue_getType(val);

    switch (type) {
        case MMS_BOOLEAN:
            return g_strdup(MmsValue_getBoolean(val) ? "TRUE (1)" : "FALSE (0)");
        case MMS_INTEGER:
            return g_strdup_printf("%lld", (long long)MmsValue_toInt64(val));
        case MMS_UNSIGNED:
            return g_strdup_printf("%llu", (unsigned long long)MmsValue_toUint32(val));
        case MMS_FLOAT:
            return g_strdup_printf("%.4f", MmsValue_toFloat(val));
        case MMS_VISIBLE_STRING:
        case MMS_STRING:
            return g_strdup_printf("\"%s\"", MmsValue_toString(val));
        case MMS_BIT_STRING: {
            int size = MmsValue_getBitStringSize(val);
            GString *bits = g_string_new("0b");
            for (int i = 0; i < size; i++) {
                g_string_append_c(bits, MmsValue_getBitStringBit(val, i) ? '1' : '0');
            }
            return g_string_free(bits, FALSE);
        }
        case MMS_UTC_TIME: {
            uint32_t timestamp = MmsValue_getUtcTimeInMs(val) / 1000;
            time_t t = (time_t)timestamp;
            struct tm *tm_info = gmtime(&t);
            char buf[64];
            if (tm_info) {
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", tm_info);
                return g_strdup(buf);
            }
            return g_strdup("Tempo UTC inválido");
        }
        case MMS_STRUCTURE:
        case MMS_ARRAY:
            return g_strdup_printf("[Estrutura/Array com %d elementos - expanda os filhos]", MmsValue_getArraySize(val));
        case MMS_DATA_ACCESS_ERROR:
            return g_strdup_printf("Erro de Acesso MMS (%d)", MmsValue_getDataAccessError(val));
        default:
            return g_strdup_printf("Tipo MMS (%d)", type);
    }
}

static MmsValue *string_to_mms_value(const char *text, MmsType type, int bitstring_size) {
    if (!text) return NULL;

    switch (type) {
        case MMS_BOOLEAN: {
            if (g_ascii_strcasecmp(text, "true") == 0 || strcmp(text, "1") == 0 || g_ascii_strcasecmp(text, "on") == 0)
                return MmsValue_newBoolean(true);
            if (g_ascii_strcasecmp(text, "false") == 0 || strcmp(text, "0") == 0 || g_ascii_strcasecmp(text, "off") == 0)
                return MmsValue_newBoolean(false);
            return NULL;
        }
        case MMS_INTEGER: {
            char *endptr;
            long long val = strtoll(text, &endptr, 10);
            if (*text == '\0' || *endptr != '\0') return NULL;
            return MmsValue_newIntegerFromInt64((int64_t)val);
        }
        case MMS_UNSIGNED: {
            char *endptr;
            unsigned long val = strtoul(text, &endptr, 10);
            if (*text == '\0' || *endptr != '\0') return NULL;
            return MmsValue_newUnsignedFromUint32((uint32_t)val);
        }
        case MMS_FLOAT: {
            char *endptr;
            float val = strtof(text, &endptr);
            if (*text == '\0' || *endptr != '\0') return NULL;
            return MmsValue_newFloat(val);
        }
        case MMS_VISIBLE_STRING:
            return MmsValue_newVisibleString(text);
        case MMS_STRING:
            return MmsValue_newMmsString(text);
        case MMS_BIT_STRING: {
            const char *bits = text;
            if (g_str_has_prefix(bits, "0b") || g_str_has_prefix(bits, "0B"))
                bits += 2;
            int len = (int)strlen(bits);
            if (len == 0) return NULL;
            int target_size = (bitstring_size > 0) ? bitstring_size : len;
            MmsValue *mms = MmsValue_newBitString(target_size);
            for (int i = 0; i < len && i < target_size; i++) {
                if (bits[i] == '1')
                    MmsValue_setBitStringBit(mms, i, true);
                else if (bits[i] == '0')
                    MmsValue_setBitStringBit(mms, i, false);
                else {
                    MmsValue_delete(mms);
                    return NULL;
                }
            }
            return mms;
        }
        default:
            return NULL;
    }
}

// ============================================================================
// 4. EXPANSÃO DINÂMICA DA ÁRVORE
// ============================================================================
static GListModel *create_child_model(gpointer item, gpointer user_data) {
    AppState *state = (AppState *)user_data;
    IedNode *node = IED_NODE(item);

    if (!state->is_connected) return NULL;

    GListStore *store = g_list_store_new(IED_TYPE_NODE);
    IedClientError error;

    g_mutex_lock(&state->net_mutex);

    if (node->type == NODE_LD) {
        LinkedList lns = IedConnection_getLogicalDeviceDirectory(state->con, &error, node->name);
        if (error == IED_ERROR_OK && lns) {
            for (LinkedList el = LinkedList_getNext(lns); el != NULL; el = LinkedList_getNext(el)) {
                char *lnName = (char *)el->data;
                char *ref = g_strdup_printf("%s/%s", node->reference, lnName);
                IedNode *child = ied_node_new(lnName, ref, NODE_LN, IEC61850_FC_NONE);
                g_list_store_append(store, child);
                g_object_unref(child);
                g_free(ref);
            }
            LinkedList_destroy(lns);
        }
    } else if (node->type == NODE_LN) {
        LinkedList dos = IedConnection_getLogicalNodeDirectory(state->con, &error, node->reference, ACSI_CLASS_DATA_OBJECT);
        if (error == IED_ERROR_OK && dos) {
            for (LinkedList el = LinkedList_getNext(dos); el != NULL; el = LinkedList_getNext(el)) {
                char *doName = (char *)el->data;
                char *ref = g_strdup_printf("%s.%s", node->reference, doName);
                IedNode *child = ied_node_new(doName, ref, NODE_DO, IEC61850_FC_NONE);
                g_list_store_append(store, child);
                g_object_unref(child);
                g_free(ref);
            }
            LinkedList_destroy(dos);
        }
    } else if (node->type == NODE_DO || node->type == NODE_DA) {
        LinkedList das = IedConnection_getDataDirectoryFC(state->con, &error, node->reference);
        if (error == IED_ERROR_OK && das) {
            for (LinkedList el = LinkedList_getNext(das); el != NULL; el = LinkedList_getNext(el)) {
                char *daNameWithFc = (char *)el->data;
                char daName[128] = {0};
                char fcStr[16] = {0};

                char *bracket = strchr(daNameWithFc, '[');
                if (bracket) {
                    size_t len = (size_t)(bracket - daNameWithFc);
                    if (len >= sizeof(daName)) len = sizeof(daName) - 1;
                    strncpy(daName, daNameWithFc, len);
                    daName[len] = '\0';
                    sscanf(bracket, "[%15[^]]]", fcStr);
                } else {
                    strncpy(daName, daNameWithFc, sizeof(daName) - 1);
                }

                FunctionalConstraint fc = (fcStr[0] != '\0') ? FunctionalConstraint_fromString(fcStr) : node->fc;
                char *ref = g_strdup_printf("%s.%s", node->reference, daName);

                IedNode *child = ied_node_new(daNameWithFc, ref, NODE_DA, fc);
                g_list_store_append(store, child);
                g_object_unref(child);
                g_free(ref);
            }
            LinkedList_destroy(das);
        }
    }

    g_mutex_unlock(&state->net_mutex);

    if (g_list_model_get_n_items(G_LIST_MODEL(store)) == 0) {
        g_object_unref(store);
        return NULL;
    }

    return G_LIST_MODEL(store);
}

// ============================================================================
// 5. LEITURA, ESCRITA/COMANDO ASSÍNCRONO E AUTO-ATUALIZAÇÃO
// ============================================================================
static void set_button_icon_and_label(GtkWidget *button, const char *icon_name, const char *label_text) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_button_set_child(GTK_BUTTON(button), box);
}

static void update_write_controls(AppState *state, gboolean can_write, gboolean is_control, MmsType type) {
    state->is_control_mode = is_control;
    gtk_widget_set_sensitive(state->entry_write, can_write);
    gtk_widget_set_sensitive(state->btn_write, can_write);

    if (is_control) {
        set_button_icon_and_label(state->btn_write, "system-run-symbolic", "Comandar (CO)");
        gtk_entry_set_placeholder_text(GTK_ENTRY(state->entry_write), "Comando: 1 (Ligar/Fechar) ou 0 (Desligar/Abrir)");
        return;
    }

    set_button_icon_and_label(state->btn_write, "document-send-symbolic", "Escrever");

    if (!can_write) {
        gtk_entry_set_placeholder_text(GTK_ENTRY(state->entry_write), "Variável somente leitura ou estrutura");
        return;
    }

    switch (type) {
        case MMS_BOOLEAN:
            gtk_entry_set_placeholder_text(GTK_ENTRY(state->entry_write), "Ex: 1, 0, true ou false");
            break;
        case MMS_INTEGER:
        case MMS_UNSIGNED:
            gtk_entry_set_placeholder_text(GTK_ENTRY(state->entry_write), "Ex: 42");
            break;
        case MMS_FLOAT:
            gtk_entry_set_placeholder_text(GTK_ENTRY(state->entry_write), "Ex: 125.50");
            break;
        case MMS_BIT_STRING:
            gtk_entry_set_placeholder_text(GTK_ENTRY(state->entry_write), "Ex: 0b0101");
            break;
        default:
            gtk_entry_set_placeholder_text(GTK_ENTRY(state->entry_write), "Digite o novo valor...");
            break;
    }
}

static void stop_auto_refresh(AppState *state) {
    if (state->auto_refresh_timer_id > 0) {
        g_source_remove(state->auto_refresh_timer_id);
        state->auto_refresh_timer_id = 0;
    }
    if (GTK_IS_TOGGLE_BUTTON(state->btn_auto_refresh)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->btn_auto_refresh), FALSE);
        set_button_icon_and_label(state->btn_auto_refresh, "media-playback-start-symbolic", "Iniciar Auto-Atualizar");
    }
    if (GTK_IS_WIDGET(state->spin_interval)) {
        gtk_widget_set_sensitive(state->spin_interval, TRUE);
    }
}

static gboolean on_read_finished(gpointer user_data) {
    ReadTaskData *task = (ReadTaskData *)user_data;
    AppState *state = task->state;

    if (state->selected_node && g_strcmp0(state->selected_node->reference, task->reference) == 0) {
        gtk_label_set_text(GTK_LABEL(state->lbl_value), task->result_text);

        if (state->selected_node->fc == IEC61850_FC_CO) {
            state->current_mms_type = MMS_BOOLEAN;
            update_write_controls(state, TRUE, TRUE, MMS_BOOLEAN);
        } else if (task->read_ok) {
            state->current_mms_type = task->mms_type;
            state->current_bitstring_size = task->bitstring_size;
            gboolean can_write = is_fc_writable(state->selected_node->fc) && is_mms_type_editable(task->mms_type);
            update_write_controls(state, can_write, FALSE, task->mms_type);
        } else {
            update_write_controls(state, FALSE, FALSE, MMS_STRUCTURE);
        }
    }

    state->is_reading = FALSE;

    g_free(task->reference);
    g_free(task->result_text);
    g_free(task);
    return G_SOURCE_REMOVE;
}

static gpointer read_thread_worker(gpointer user_data) {
    ReadTaskData *task = (ReadTaskData *)user_data;
    AppState *state = task->state;
    g_mutex_lock(&state->net_mutex);

    if (state->is_connected && state->con) {
        if (task->fc == IEC61850_FC_CO) {
            task->read_ok = FALSE;
            task->result_text = g_strdup("(Objeto de Controle FC=CO: digite 1 ou 0 abaixo para comandar)");
        } else {
            IedClientError error;
            MmsValue *val = IedConnection_readObject(state->con, &error, task->reference, task->fc);

            if (error == IED_ERROR_OK && val != NULL) {
                task->read_ok = TRUE;
                task->mms_type = MmsValue_getType(val);
                if (task->mms_type == MMS_BIT_STRING) {
                    task->bitstring_size = MmsValue_getBitStringSize(val);
                }
                task->result_text = mms_value_to_string(val);
                MmsValue_delete(val);
            } else {
                task->read_ok = FALSE;
                task->result_text = g_strdup_printf("Erro de leitura (Código: %d)", error);
            }
        }
    } else {
        task->read_ok = FALSE;
        task->result_text = g_strdup("Desconectado");
    }

    g_mutex_unlock(&state->net_mutex);
    g_idle_add(on_read_finished, task);
    return NULL;
}

static void trigger_async_read(AppState *state) {
    if (!state->selected_node || state->is_reading)
        return;

    IedNode *node = state->selected_node;
    gtk_label_set_text(GTK_LABEL(state->lbl_ref), node->reference);
    const char *type_names[] = {"Logical Device (LD)", "Logical Node (LN)", "Data Object (DO)", "Data Attribute / Sub-DO"};
    gtk_label_set_text(GTK_LABEL(state->lbl_type), type_names[node->type]);

    if (node->fc != IEC61850_FC_NONE) {
        gtk_label_set_text(GTK_LABEL(state->lbl_fc), FunctionalConstraint_toString(node->fc));
    } else {
        gtk_label_set_text(GTK_LABEL(state->lbl_fc), "N/A");
    }

    if (state->is_connected && node->fc == IEC61850_FC_CO) {
        stop_auto_refresh(state);
        gtk_widget_set_sensitive(state->btn_refresh, FALSE);
        gtk_widget_set_sensitive(state->btn_auto_refresh, FALSE);
        gtk_label_set_text(GTK_LABEL(state->lbl_value), "(Objeto de Controle FC=CO: digite 1 ou 0 abaixo para comandar)");
        state->current_mms_type = MMS_BOOLEAN;
        update_write_controls(state, TRUE, TRUE, MMS_BOOLEAN);
        return;
    }

    if (state->is_connected && node->type == NODE_DA && node->fc != IEC61850_FC_NONE) {
        state->is_reading = TRUE;
        gtk_widget_set_sensitive(state->btn_refresh, TRUE);
        gtk_widget_set_sensitive(state->btn_auto_refresh, TRUE);

        ReadTaskData *task = g_new0(ReadTaskData, 1);
        task->state = state;
        task->reference = g_strdup(node->reference);
        task->fc = node->fc;
        g_thread_new("mms-read", read_thread_worker, task);
    } else {
        stop_auto_refresh(state);
        update_write_controls(state, FALSE, FALSE, MMS_STRUCTURE);
        gtk_label_set_text(GTK_LABEL(state->lbl_value), "(Selecione um Data Attribute folha para ler ou um nó [CO] para comandar)");
        gtk_widget_set_sensitive(state->btn_refresh, FALSE);
        gtk_widget_set_sensitive(state->btn_auto_refresh, FALSE);
    }
}

// --- Lógica de Escrita e Comando IEC 61850 ---
static gboolean on_write_finished(gpointer user_data) {
    WriteTaskData *task = (WriteTaskData *)user_data;
    AppState *state = task->state;

    if (!task->parse_ok) {
        gtk_label_set_text(GTK_LABEL(state->lbl_status), "Status: Formato inválido. Para comandos use 1 (ON/Fechar) ou 0 (OFF/Abrir).");
    } else if (task->is_control) {
        if (task->control_success) {
            gtk_label_set_text(GTK_LABEL(state->lbl_status), task->detail_msg);
            gtk_editable_set_text(GTK_EDITABLE(state->entry_write), "");
        } else {
            gtk_label_set_text(GTK_LABEL(state->lbl_status), task->detail_msg);
        }
    } else if (task->error == IED_ERROR_OK) {
        char *msg = g_strdup_printf("Status: Valor escrito com sucesso em %s", task->reference);
        gtk_label_set_text(GTK_LABEL(state->lbl_status), msg);
        g_free(msg);
        gtk_editable_set_text(GTK_EDITABLE(state->entry_write), "");
        trigger_async_read(state);
    } else {
        char *msg = g_strdup_printf("Status: Falha ao escrever em %s (Erro libiec61850: %d)", task->reference, task->error);
        gtk_label_set_text(GTK_LABEL(state->lbl_status), msg);
        g_free(msg);
    }

    if (state->selected_node && (is_fc_writable(state->selected_node->fc) || state->selected_node->fc == IEC61850_FC_CO)) {
        gtk_widget_set_sensitive(state->btn_write, TRUE);
    }

    g_free(task->reference);
    g_free(task->input_text);
    g_free(task->detail_msg);
    g_free(task);
    return G_SOURCE_REMOVE;
}

static gpointer write_thread_worker(gpointer user_data) {
    WriteTaskData *task = (WriteTaskData *)user_data;
    AppState *state = task->state;

    MmsValue *new_val = string_to_mms_value(task->input_text, task->mms_type, task->bitstring_size);
    if (!new_val) {
        task->parse_ok = FALSE;
        g_idle_add(on_write_finished, task);
        return NULL;
    }

    task->parse_ok = TRUE;
    g_mutex_lock(&state->net_mutex);

    if (!state->is_connected || !state->con) {
        task->error = IED_ERROR_NOT_CONNECTED;
        task->control_success = FALSE;
        task->detail_msg = g_strdup("Status: Desconectado do IED.");
    } else if (task->is_control) {
        char *co_ref = extract_control_object_ref(task->reference);
        ControlObjectClient control = ControlObjectClient_create(co_ref, state->con);

        if (control) {
            ControlModel model = ControlObjectClient_getControlModel(control);
            bool ok = false;
            const char *mode_str = "Direct";

            if (model == CONTROL_MODEL_SBO_NORMAL) {
                mode_str = "SBO Normal";
                if (ControlObjectClient_select(control)) {
                    ok = ControlObjectClient_operate(control, new_val, 0);
                }
            } else if (model == CONTROL_MODEL_SBO_ENHANCED) {
                mode_str = "SBO Enhanced";
                if (ControlObjectClient_selectWithValue(control, new_val)) {
                    ok = ControlObjectClient_operate(control, new_val, 0);
                }
            } else {
                mode_str = (model == CONTROL_MODEL_DIRECT_ENHANCED) ? "Direct Enhanced" : "Direct Normal";
                ok = ControlObjectClient_operate(control, new_val, 0);
            }

            task->control_success = ok ? TRUE : FALSE;
            if (ok) {
                task->detail_msg = g_strdup_printf("Status: Comando [%s] enviado com sucesso para %s (Verifique stVal[ST])", mode_str, co_ref);
            } else {
                LastApplError last_err = ControlObjectClient_getLastApplError(control);
                task->detail_msg = g_strdup_printf("Status: Falha no comando [%s] em %s (addCause: %d)", mode_str, co_ref, last_err.addCause);
            }

            ControlObjectClient_destroy(control);
        } else {
            task->control_success = FALSE;
            task->detail_msg = g_strdup_printf("Status: Não foi possível criar ControlObjectClient para %s", co_ref);
        }
        g_free(co_ref);
    } else {
        IedConnection_writeObject(state->con, &task->error, task->reference, task->fc, new_val);
    }

    g_mutex_unlock(&state->net_mutex);
    MmsValue_delete(new_val);

    g_idle_add(on_write_finished, task);
    return NULL;
}

static void on_write_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppState *state = (AppState *)user_data;

    if (!state->is_connected || !state->selected_node)
        return;

    const char *text = gtk_editable_get_text(GTK_EDITABLE(state->entry_write));
    if (!text || strlen(text) == 0)
        return;

    gtk_widget_set_sensitive(state->btn_write, FALSE);

    WriteTaskData *task = g_new0(WriteTaskData, 1);
    task->state = state;
    task->reference = g_strdup(state->selected_node->reference);
    task->fc = state->selected_node->fc;
    task->input_text = g_strdup(text);
    task->mms_type = state->current_mms_type;
    task->bitstring_size = state->current_bitstring_size;
    task->is_control = state->is_control_mode;

    g_thread_new("mms-write", write_thread_worker, task);
}

static gboolean on_auto_refresh_tick(gpointer user_data) {
    AppState *state = (AppState *)user_data;

    if (!state->is_connected || !state->selected_node) {
        stop_auto_refresh(state);
        return G_SOURCE_REMOVE;
    }

    trigger_async_read(state);
    return G_SOURCE_CONTINUE;
}

static void on_auto_refresh_toggled(GtkToggleButton *btn, gpointer user_data) {
    AppState *state = (AppState *)user_data;

    if (gtk_toggle_button_get_active(btn)) {
        guint interval_ms = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(state->spin_interval));
        if (interval_ms < 50)
            interval_ms = 50;

        gtk_widget_set_sensitive(state->spin_interval, FALSE);
        set_button_icon_and_label(GTK_WIDGET(btn), "media-playback-stop-symbolic", "Parar Auto-Atualizar");
        trigger_async_read(state);
        state->auto_refresh_timer_id = g_timeout_add(interval_ms, on_auto_refresh_tick, state);
    } else {
        stop_auto_refresh(state);
    }
}

static void on_selection_changed(GtkSelectionModel *model, guint position, guint n_items, gpointer user_data) {
    (void)position;
    (void)n_items;
    AppState *state = (AppState *)user_data;
    GtkTreeListRow *row = gtk_single_selection_get_selected_item(GTK_SINGLE_SELECTION(model));

    if (row) {
        state->selected_node = IED_NODE(gtk_tree_list_row_get_item(row));
        gtk_editable_set_text(GTK_EDITABLE(state->entry_write), "");
        update_write_controls(state, FALSE, FALSE, MMS_STRUCTURE);
        trigger_async_read(state);
    }
}

static void on_refresh_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppState *state = (AppState *)user_data;
    trigger_async_read(state);
}

// ============================================================================
// 6. RENDERIZAÇÃO DAS LINHAS DA ÁRVORE COM ÍCONES NATIVOS
// ============================================================================
static void setup_tree_item(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
    (void)factory;
    (void)user_data;
    GtkWidget *expander = gtk_tree_expander_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon = gtk_image_new();
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
    gtk_list_item_set_child(list_item, expander);
}

static void bind_tree_item(GtkListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
    (void)factory;
    (void)user_data;
    GtkTreeListRow *row = gtk_list_item_get_item(list_item);
    IedNode *node = IED_NODE(gtk_tree_list_row_get_item(row));
    GtkWidget *expander = gtk_list_item_get_child(list_item);
    gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);
    GtkWidget *box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander));
    GtkWidget *icon = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_next_sibling(icon);
    const char *icon_name = "emblem-system-symbolic";

    if (node->type == NODE_LD)
        icon_name = "drive-harddisk-symbolic";
    else if (node->type == NODE_LN)
        icon_name = "text-x-generic-symbolic";
    else if (node->type == NODE_DO)
        icon_name = "folder-symbolic";
    else if (node->fc == IEC61850_FC_CO)
        icon_name = "system-run-symbolic";

    gtk_image_set_from_icon_name(GTK_IMAGE(icon), icon_name);
    gtk_label_set_text(GTK_LABEL(label), node->name);
}

// ============================================================================
// 7. CONEXÃO ASSÍNCRONA E DESCONEXÃO
// ============================================================================
static gboolean on_connect_finished(gpointer user_data) {
    ConnectTaskData *task = (ConnectTaskData *)user_data;
    AppState *state = task->state;

    if (task->error == IED_ERROR_OK) {
        g_mutex_lock(&state->net_mutex);
        state->con = task->con;
        state->is_connected = TRUE;
        g_mutex_unlock(&state->net_mutex);

        if (task->device_list) {
            for (LinkedList el = LinkedList_getNext(task->device_list); el != NULL; el = LinkedList_getNext(el)) {
                char *ldName = (char *)el->data;
                IedNode *ldNode = ied_node_new(ldName, ldName, NODE_LD, IEC61850_FC_NONE);
                g_list_store_append(state->root_store, ldNode);
                g_object_unref(ldNode);
            }
            LinkedList_destroy(task->device_list);
        }

        char *status_msg = g_strdup_printf("Status: Conectado a %s:%d", task->ip, task->port);
        gtk_label_set_text(GTK_LABEL(state->lbl_status), status_msg);
        g_free(status_msg);
        gtk_widget_set_sensitive(state->btn_connect, FALSE);
        gtk_widget_set_sensitive(state->btn_disconnect, TRUE);
    } else {
        if (task->con) IedConnection_destroy(task->con);
        char *err_msg = g_strdup_printf("Status: Falha ao conectar em %s:%d (Erro libiec61850: %d)", task->ip, task->port, task->error);
        gtk_label_set_text(GTK_LABEL(state->lbl_status), err_msg);
        g_free(err_msg);
        gtk_widget_set_sensitive(state->btn_connect, TRUE);
    }
    g_free(task->ip);
    g_free(task);
    return G_SOURCE_REMOVE;
}

static gpointer connect_thread_worker(gpointer user_data) {
    ConnectTaskData *task = (ConnectTaskData *)user_data;
    task->con = IedConnection_create();
    IedConnection_setConnectTimeout(task->con, 3000);
    IedConnection_connect(task->con, &task->error, task->ip, task->port);

    if (task->error == IED_ERROR_OK) {
        task->device_list = IedConnection_getLogicalDeviceList(task->con, &task->error);
    }

    g_idle_add(on_connect_finished, task);
    return NULL;
}

static void on_disconnect_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppState *state = (AppState *)user_data;
    stop_auto_refresh(state);
    g_mutex_lock(&state->net_mutex);

    if (state->is_connected && state->con) {
        IedConnection_close(state->con);
        IedConnection_destroy(state->con);
        state->con = NULL;
        state->is_connected = FALSE;
    }

    g_mutex_unlock(&state->net_mutex);
    g_list_store_remove_all(state->root_store);
    state->selected_node = NULL;
    update_write_controls(state, FALSE, FALSE, MMS_STRUCTURE);
    gtk_editable_set_text(GTK_EDITABLE(state->entry_write), "");
    gtk_widget_set_sensitive(state->btn_connect, TRUE);
    gtk_widget_set_sensitive(state->btn_disconnect, FALSE);
    gtk_widget_set_sensitive(state->btn_refresh, FALSE);
    gtk_widget_set_sensitive(state->btn_auto_refresh, FALSE);
    gtk_label_set_text(GTK_LABEL(state->lbl_status), "Status: Desconectado");
    gtk_label_set_text(GTK_LABEL(state->lbl_value), "-");
}

static void on_connect_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppState *state = (AppState *)user_data;
    const char *ip = gtk_editable_get_text(GTK_EDITABLE(state->entry_ip));
    int port = atoi(gtk_editable_get_text(GTK_EDITABLE(state->entry_port)));
    gtk_widget_set_sensitive(state->btn_connect, FALSE);
    char *msg = g_strdup_printf("Status: Conectando a %s:%d...", ip, port);
    gtk_label_set_text(GTK_LABEL(state->lbl_status), msg);
    g_free(msg);
    ConnectTaskData *task = g_new0(ConnectTaskData, 1);
    task->state = state;
    task->ip = g_strdup(ip);
    task->port = port;
    g_thread_new("mms-connect", connect_thread_worker, task);
}

// ============================================================================
// 8. CONSTRUÇÃO DA INTERFACE GRÁFICA
// ============================================================================
static void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    AppState *state = g_new0(AppState, 1);
    g_mutex_init(&state->net_mutex);
    state->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(state->window), "Mini IED Explorer - Browser, Leitura, Escrita & Controle MMS");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 980, 600);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_window_set_child(GTK_WINDOW(state->window), vbox);

    // --- Barra Superior de Conexão ---
    GtkWidget *hbox_conn = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(vbox), hbox_conn);
    state->entry_ip = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(state->entry_ip), "127.0.0.1");
    state->entry_port = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(state->entry_port), "102");
    gtk_widget_set_size_request(state->entry_port, 70, -1);
    state->btn_connect = gtk_button_new();
    set_button_icon_and_label(state->btn_connect, "network-transmit-receive-symbolic", "Conectar");
    g_signal_connect(state->btn_connect, "clicked", G_CALLBACK(on_connect_clicked), state);
    state->btn_disconnect = gtk_button_new();
    set_button_icon_and_label(state->btn_disconnect, "network-offline-symbolic", "Desconectar");
    gtk_widget_set_sensitive(state->btn_disconnect, FALSE);
    g_signal_connect(state->btn_disconnect, "clicked", G_CALLBACK(on_disconnect_clicked), state);
    gtk_box_append(GTK_BOX(hbox_conn), gtk_label_new("IP:"));
    gtk_box_append(GTK_BOX(hbox_conn), state->entry_ip);
    gtk_box_append(GTK_BOX(hbox_conn), gtk_label_new("Porta:"));
    gtk_box_append(GTK_BOX(hbox_conn), state->entry_port);
    gtk_box_append(GTK_BOX(hbox_conn), state->btn_connect);
    gtk_box_append(GTK_BOX(hbox_conn), state->btn_disconnect);

    // --- Painel Dividido ---
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_paned_set_position(GTK_PANED(paned), 420);
    gtk_box_append(GTK_BOX(vbox), paned);
    state->root_store = g_list_store_new(IED_TYPE_NODE);
    state->tree_model = gtk_tree_list_model_new(G_LIST_MODEL(state->root_store), FALSE, FALSE, create_child_model, state, NULL);
    state->selection_model = gtk_single_selection_new(G_LIST_MODEL(state->tree_model));
    g_signal_connect(state->selection_model, "selection-changed", G_CALLBACK(on_selection_changed), state);
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(setup_tree_item), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(bind_tree_item), NULL);
    GtkWidget *list_view = gtk_list_view_new(GTK_SELECTION_MODEL(state->selection_model), factory);
    GtkWidget *scrolled_tree = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_tree), list_view);
    gtk_paned_set_start_child(GTK_PANED(paned), scrolled_tree);

    // Direita: Painel de Propriedades
    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start(right_box, 16);
    gtk_widget_set_margin_top(right_box, 8);
    gtk_paned_set_end_child(GTK_PANED(paned), right_box);
    GtkWidget *title = gtk_label_new("Inspeção do Objeto IEC 61850");
    gtk_label_set_use_markup(GTK_LABEL(title), TRUE);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_append(GTK_BOX(right_box), title);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_append(GTK_BOX(right_box), grid);

    state->lbl_ref = gtk_label_new("-");
    state->lbl_type = gtk_label_new("-");
    state->lbl_fc = gtk_label_new("-");
    state->lbl_value = gtk_label_new("-");
    gtk_label_set_selectable(GTK_LABEL(state->lbl_ref), TRUE);
    gtk_label_set_selectable(GTK_LABEL(state->lbl_value), TRUE);
    gtk_label_set_xalign(GTK_LABEL(state->lbl_ref), 0.0);
    gtk_label_set_xalign(GTK_LABEL(state->lbl_type), 0.0);
    gtk_label_set_xalign(GTK_LABEL(state->lbl_fc), 0.0);
    gtk_label_set_xalign(GTK_LABEL(state->lbl_value), 0.0);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Referência:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), state->lbl_ref, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Nível Hierárquico:"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), state->lbl_type, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Functional Constraint (FC):"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), state->lbl_fc, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Valor Atual (MMS):"), 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), state->lbl_value, 1, 3, 1, 1);

    // --- Seção de Escrita e Comando ---
    gtk_box_append(GTK_BOX(right_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    GtkWidget *lbl_write_title = gtk_label_new("Escrita de Parâmetro / Comando de Controle");
    gtk_label_set_xalign(GTK_LABEL(lbl_write_title), 0.0);
    gtk_box_append(GTK_BOX(right_box), lbl_write_title);

    GtkWidget *hbox_write = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(right_box), hbox_write);

    state->entry_write = gtk_entry_new();
    gtk_widget_set_hexpand(state->entry_write, TRUE);
    gtk_widget_set_sensitive(state->entry_write, FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->entry_write), "Selecione uma variável editável ou controle [CO]...");

    state->btn_write = gtk_button_new();
    set_button_icon_and_label(state->btn_write, "document-send-symbolic", "Escrever");
    gtk_widget_set_sensitive(state->btn_write, FALSE);
    g_signal_connect(state->btn_write, "clicked", G_CALLBACK(on_write_clicked), state);

    gtk_box_append(GTK_BOX(hbox_write), state->entry_write);
    gtk_box_append(GTK_BOX(hbox_write), state->btn_write);

    // --- Barra de Controles de Atualização (Manual + Automática) ---
    gtk_box_append(GTK_BOX(right_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    GtkWidget *hbox_refresh = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(right_box), hbox_refresh);
    state->btn_refresh = gtk_button_new();
    set_button_icon_and_label(state->btn_refresh, "view-refresh-symbolic", "Ler Agora");
    gtk_widget_set_sensitive(state->btn_refresh, FALSE);
    g_signal_connect(state->btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked), state);
    state->spin_interval = gtk_spin_button_new_with_range(50.0, 60000.0, 100.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->spin_interval), 1000.0);
    state->btn_auto_refresh = gtk_toggle_button_new();
    set_button_icon_and_label(state->btn_auto_refresh, "media-playback-start-symbolic", "Iniciar Auto-Atualizar");
    gtk_widget_set_sensitive(state->btn_auto_refresh, FALSE);
    g_signal_connect(state->btn_auto_refresh, "toggled", G_CALLBACK(on_auto_refresh_toggled), state);
    gtk_box_append(GTK_BOX(hbox_refresh), state->btn_refresh);
    gtk_box_append(GTK_BOX(hbox_refresh), gtk_label_new("Intervalo (ms):"));
    gtk_box_append(GTK_BOX(hbox_refresh), state->spin_interval);
    gtk_box_append(GTK_BOX(hbox_refresh), state->btn_auto_refresh);

    // --- Barra de Status Inferior ---
    state->lbl_status = gtk_label_new("Status: Desconectado");
    gtk_label_set_xalign(GTK_LABEL(state->lbl_status), 0.0);
    gtk_box_append(GTK_BOX(vbox), state->lbl_status);
    gtk_window_present(GTK_WINDOW(state->window));
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("br.com.automacao.miniiedexplorer", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
