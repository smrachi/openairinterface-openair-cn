/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under 
 * the Apache License, Version 2.0  (the "License"); you may not use this file
 * except in compliance with the License.  
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*****************************************************************************
  Source      SecurityModeControl.c

  Version     0.1

  Date        2013/04/22

  Product     NAS stack

  Subsystem   Template body file

  Author      Frederic Maurel

  Description Defines the security mode control EMM procedure executed by the
        Non-Access Stratum.

        The purpose of the NAS security mode control procedure is to
        take an EPS security context into use, and initialise and start
        NAS signalling security between the UE and the MME with the
        corresponding EPS NAS keys and EPS security algorithms.

        Furthermore, the network may also initiate a SECURITY MODE COM-
        MAND in order to change the NAS security algorithms for a cur-
        rent EPS security context already in use.

*****************************************************************************/

#include <stdlib.h>             // MALLOC_CHECK, FREE_CHECK
#include <string.h>             // memcpy
#include <inttypes.h>

#include "emm_proc.h"
#include "log.h"
#include "nas_timer.h"

#include "emmData.h"

#include "emm_sap.h"
#include "emm_cause.h"

#include "UeSecurityCapability.h"

#if ENABLE_ITTI
#  include "assertions.h"
#endif
#include "secu_defs.h"
#include "msc.h"
#include "dynamic_memory_check.h"

/****************************************************************************/
/****************  E X T E R N A L    D E F I N I T I O N S  ****************/
/****************************************************************************/

/****************************************************************************/
/*******************  L O C A L    D E F I N I T I O N S  *******************/
/****************************************************************************/

/*
   --------------------------------------------------------------------------
    Internal data handled by the security mode control procedure in the UE
   --------------------------------------------------------------------------
*/

/*
   --------------------------------------------------------------------------
    Internal data handled by the security mode control procedure in the MME
   --------------------------------------------------------------------------
*/
/*
   Timer handlers
*/
static void                            *_security_t3460_handler (
  void *);

/*
   Function executed whenever the ongoing EMM procedure that initiated
   the security mode control procedure is aborted or the maximum value of the
   retransmission timer counter is exceed
*/
static int                              _security_abort (
  void *);
static int                              _security_select_algorithms (
  const int ue_eiaP,
  const int ue_eeaP,
  int *const mme_eiaP,
  int *const mme_eeaP);

/*
   Internal data used for security mode control procedure
*/
typedef struct {
  unsigned int                            ue_id; /* UE identifier                         */
#define SECURITY_COUNTER_MAX    5
  unsigned int                            retransmission_count; /* Retransmission counter    */
  int                                     ksi;  /* NAS key set identifier                */
  int                                     eea;  /* Replayed EPS encryption algorithms    */
  int                                     eia;  /* Replayed EPS integrity algorithms     */
  int                                     ucs2; /* Replayed Alphabet                     */
  int                                     uea;  /* Replayed UMTS encryption algorithms   */
  int                                     uia;  /* Replayed UMTS integrity algorithms    */
  int                                     gea;  /* Replayed G encryption algorithms      */
  int                                     umts_present:1;
  int                                     gprs_present:1;
  int                                     selected_eea; /* Selected EPS encryption algorithms    */
  int                                     selected_eia; /* Selected EPS integrity algorithms     */
  int                                     notify_failure;       /* Indicates whether the security mode control
                                                                 * procedure failure shall be notified to the
                                                                 * ongoing EMM procedure        */
} security_data_t;

static int                              _security_request (
  security_data_t * data,
  int is_new);

/****************************************************************************/
/******************  E X P O R T E D    F U N C T I O N S  ******************/
/****************************************************************************/


/*
   --------------------------------------------------------------------------
        Security mode control procedure executed by the MME
   --------------------------------------------------------------------------
*/


/****************************************************************************
 **                                                                        **
 ** Name:    emm_proc_security_mode_control()                          **
 **                                                                        **
 ** Description: Initiates the security mode control procedure.            **
 **                                                                        **
 **              3GPP TS 24.301, section 5.4.3.2                           **
 **      The MME initiates the NAS security mode control procedure **
 **      by sending a SECURITY MODE COMMAND message to the UE and  **
 **      starting timer T3460. The message shall be sent unciphe-  **
 **      red but shall be integrity protected using the NAS inte-  **
 **      grity key based on KASME.                                 **
 **                                                                        **
 ** Inputs:  ue_id:      UE lower layer identifier                  **
 **      ksi:       NAS key set identifier                     **
 **      eea:       Replayed EPS encryption algorithms         **
 **      eia:       Replayed EPS integrity algorithms          **
 **      success:   Callback function executed when the secu-  **
 **             rity mode control procedure successfully   **
 **             completes                                  **
 **      reject:    Callback function executed when the secu-  **
 **             rity mode control procedure fails or is    **
 **             rejected                                   **
 **      failure:   Callback function executed whener a lower  **
 **             layer failure occured before the security  **
 **             mode control procedure comnpletes          **
 **      Others:    None                                       **
 **                                                                        **
 ** Outputs:     None                                                      **
 **      Return:    RETURNok, RETURNerror                      **
 **      Others:    None                                       **
 **                                                                        **
 ***************************************************************************/
int
emm_proc_security_mode_control (
  mme_ue_s1ap_id_t ue_id,
  int ksi,
  int eea,
  int eia,
  int ucs2,
  int uea,
  int uia,
  int gea,
  int umts_present,
  int gprs_present,
  emm_common_success_callback_t success,
  emm_common_reject_callback_t reject,
  emm_common_failure_callback_t failure)
{
  int                                     rc = RETURNerror;
  int                                     security_context_is_new = false;
  int                                     mme_eea = NAS_SECURITY_ALGORITHMS_EEA0;
  int                                     mme_eia = NAS_SECURITY_ALGORITHMS_EIA0;

  /*
   * Get the UE context
   */
  emm_data_context_t                     *emm_ctx = NULL;

  LOG_FUNC_IN (LOG_NAS_EMM);
  LOG_INFO (LOG_NAS_EMM, "EMM-PROC  - Initiate security mode control procedure " "KSI = %d EEA = %d EIA = %d\n", ksi, eea, eia);

  if (ue_id > 0) {
    emm_ctx = emm_data_context_get (&_emm_data, ue_id);
  }


  if (emm_ctx && emm_ctx->security) {
    if (emm_ctx->security->type == EMM_KSI_NOT_AVAILABLE) {
      /*
       * The security mode control procedure is initiated to take into use
       * * * * the EPS security context created after a successful execution of
       * * * * the EPS authentication procedure
       */
      emm_ctx->security->type = EMM_KSI_NATIVE;
      emm_ctx->security->eksi = ksi;
      emm_ctx->security->dl_count.overflow = 0;
      emm_ctx->security->dl_count.seq_num = 0;
      /*
       * TODO !!! Compute Kasme, and NAS cyphering and integrity keys
       */
      // LG: Kasme should have been received from authentication
      //     information request (S6A)
      // Kasme is located in emm_ctx->vector.kasme
      FREE_OCTET_STRING (emm_ctx->security->kasme);
      emm_ctx->security->kasme.value = CALLOC_CHECK (32, sizeof(uint8_t));
      memcpy (emm_ctx->security->kasme.value, emm_ctx->vector.kasme, 32);
      emm_ctx->security->kasme.length = 32;
      rc = _security_select_algorithms (eia, eea, &mme_eia, &mme_eea);
      emm_ctx->security->selected_algorithms.encryption = mme_eea;
      emm_ctx->security->selected_algorithms.integrity = mme_eia;

      if (rc == RETURNerror) {
        LOG_WARNING (LOG_NAS_EMM, "EMM-PROC  - Failed to select security algorithms\n");
        LOG_FUNC_RETURN (LOG_NAS_EMM, RETURNerror);
      }

      if (!emm_ctx->security->knas_int.value) {
        emm_ctx->security->knas_int.value = CALLOC_CHECK (AUTH_KNAS_INT_SIZE, sizeof(uint8_t));
      } else {
        LOG_ERROR (LOG_NAS_EMM, " TODO realloc emm_ctx->security->knas_int OctetString\n");
        LOG_FUNC_RETURN (LOG_NAS_EMM, RETURNerror);
      }

      emm_ctx->security->knas_int.length = AUTH_KNAS_INT_SIZE;
      derive_key_nas (NAS_INT_ALG, emm_ctx->security->selected_algorithms.integrity, &emm_ctx->vector.kasme[0], emm_ctx->security->knas_int.value);

      if (!emm_ctx->security->knas_enc.value) {
        emm_ctx->security->knas_enc.value = CALLOC_CHECK (AUTH_KNAS_ENC_SIZE, sizeof(uint8_t));
      } else {
        LOG_ERROR (LOG_NAS_EMM, " TODO realloc emm_ctx->security->knas_enc OctetString\n");
        LOG_FUNC_RETURN (LOG_NAS_EMM, RETURNerror);
      }

      emm_ctx->security->knas_enc.length = AUTH_KNAS_ENC_SIZE;
      derive_key_nas (NAS_ENC_ALG, emm_ctx->security->selected_algorithms.encryption, &emm_ctx->vector.kasme[0], emm_ctx->security->knas_enc.value);
      /*
       * Set new security context indicator
       */
      security_context_is_new = true;
    }
  } else {
    LOG_WARNING (LOG_NAS_EMM, "EMM-PROC  - No EPS security context exists\n");
    LOG_FUNC_RETURN (LOG_NAS_EMM, RETURNerror);
  }

  /*
   * Allocate parameters of the retransmission timer callback
   */
  security_data_t                        *data = (security_data_t *) MALLOC_CHECK (sizeof (security_data_t));

  if (data ) {
    /*
     * Setup ongoing EMM procedure callback functions
     */
    rc = emm_proc_common_initialize (ue_id, success, reject, failure, _security_abort, data);

    if (rc != RETURNok) {
      LOG_WARNING (LOG_NAS_EMM, "Failed to initialize EMM callback functions\n");
      FREE_CHECK (data);
      LOG_FUNC_RETURN (LOG_NAS_EMM, RETURNerror);
    }

    /*
     * Set the UE identifier
     */
    data->ue_id = ue_id;
    /*
     * Reset the retransmission counter
     */
    data->retransmission_count = 0;
    /*
     * Set the key set identifier
     */
    data->ksi = ksi;
    /*
     * Set the EPS encryption algorithms to be replayed to the UE
     */
    data->eea = eea;
    /*
     * Set the EPS integrity algorithms to be replayed to the UE
     */
    data->eia = eia;
    data->ucs2 = ucs2;
    /*
     * Set the UMTS encryption algorithms to be replayed to the UE
     */
    data->uea = uea;
    /*
     * Set the UMTS integrity algorithms to be replayed to the UE
     */
    data->uia = uia;
    /*
     * Set the GPRS integrity algorithms to be replayed to the UE
     */
    data->gea = gea;
    data->umts_present = umts_present;
    data->gprs_present = gprs_present;
    /*
     * Set the EPS encryption algorithms selected to the UE
     */
    data->selected_eea = emm_ctx->security->selected_algorithms.encryption;
    /*
     * Set the EPS integrity algorithms selected to the UE
     */
    data->selected_eia = emm_ctx->security->selected_algorithms.integrity;
    /*
     * Set the failure notification indicator
     */
    data->notify_failure = false;
    /*
     * Send security mode command message to the UE
     */
    rc = _security_request (data, security_context_is_new);

    if (rc != RETURNerror) {
      /*
       * Notify EMM that common procedure has been initiated
       */
      MSC_LOG_TX_MESSAGE (MSC_NAS_EMM_MME, MSC_NAS_EMM_MME, NULL, 0, "EMMREG_COMMON_PROC_REQ ue id " MME_UE_S1AP_ID_FMT " (security mode control)",ue_id);
      emm_sap_t                               emm_sap = {0};

      emm_sap.primitive = EMMREG_COMMON_PROC_REQ;
      emm_sap.u.emm_reg.ue_id = ue_id;
      emm_sap.u.emm_reg.ctx = emm_ctx;
      rc = emm_sap_send (&emm_sap);
    }
  }

  LOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

/****************************************************************************
 **                                                                        **
 ** Name:    emm_proc_security_mode_complete()                         **
 **                                                                        **
 ** Description: Performs the security mode control completion procedure   **
 **      executed by the network.                                  **
 **                                                                        **
 **              3GPP TS 24.301, section 5.4.3.4                           **
 **      Upon receiving the SECURITY MODE COMPLETE message, the    **
 **      MME shall stop timer T3460.                               **
 **      From this time onward the MME shall integrity protect and **
 **      encipher all signalling messages with the selected NAS    **
 **      integrity and ciphering algorithms.                       **
 **                                                                        **
 ** Inputs:  ue_id:      UE lower layer identifier                  **
 **      Others:    None                                       **
 **                                                                        **
 ** Outputs:     None                                                      **
 **      Return:    RETURNok, RETURNerror                      **
 **      Others:    None                                       **
 **                                                                        **
 ***************************************************************************/
int
emm_proc_security_mode_complete (
  mme_ue_s1ap_id_t ue_id)
{
  emm_data_context_t                     *emm_ctx = NULL;
  int                                     rc = RETURNerror;
  emm_sap_t                               emm_sap = {0};

  LOG_FUNC_IN (LOG_NAS_EMM);
  LOG_INFO (LOG_NAS_EMM, "EMM-PROC  - Security mode complete (ue_id=" MME_UE_S1AP_ID_FMT ")\n", ue_id);
  /*
   * Get the UE context
   */

  if (ue_id > 0) {
    emm_ctx = emm_data_context_get (&_emm_data, ue_id);
  }

  if (emm_ctx) {
    /*
     * Stop timer T3460
     */
    LOG_INFO (LOG_NAS_EMM, "EMM-PROC  - Stop timer T3460 (%d)\n", emm_ctx->T3460.id);
    emm_ctx->T3460.id = nas_timer_stop (emm_ctx->T3460.id);
    MSC_LOG_EVENT (MSC_NAS_EMM_MME, "T3460 stopped UE " MME_UE_S1AP_ID_FMT " ", ue_id);
  }

  /*
   * Release retransmission timer paramaters
   */
  security_data_t                        *data = (security_data_t *) (emm_proc_common_get_args (ue_id));

  if (data) {
    FREE_CHECK (data);
  }

  if (emm_ctx && emm_ctx->security) {
    /*
     * Notify EMM that the authentication procedure successfully completed
     */
    MSC_LOG_TX_MESSAGE (MSC_NAS_EMM_MME, MSC_NAS_EMM_MME, NULL, 0, "EMMREG_COMMON_PROC_CNF ue id " MME_UE_S1AP_ID_FMT " (security mode complete)", ue_id);
    emm_sap.primitive = EMMREG_COMMON_PROC_CNF;
    emm_sap.u.emm_reg.ue_id = ue_id;
    emm_sap.u.emm_reg.ctx = emm_ctx;
    emm_sap.u.emm_reg.u.common.is_attached = emm_ctx->is_attached;
  } else {
    LOG_ERROR (LOG_NAS_EMM, "EMM-PROC  - No EPS security context exists\n");
    /*
     * Notify EMM that the authentication procedure failed
     */
    MSC_LOG_TX_MESSAGE (MSC_NAS_EMM_MME, MSC_NAS_EMM_MME, NULL, 0, "EMMREG_COMMON_PROC_REJ ue id " MME_UE_S1AP_ID_FMT " (security mode complete)", ue_id);
    emm_sap.primitive = EMMREG_COMMON_PROC_REJ;
    emm_sap.u.emm_reg.ue_id = ue_id;
    emm_sap.u.emm_reg.ctx = emm_ctx;
  }

  rc = emm_sap_send (&emm_sap);
  LOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

/****************************************************************************
 **                                                                        **
 ** Name:    emm_proc_security_mode_reject()                           **
 **                                                                        **
 ** Description: Performs the security mode control not accepted by the UE **
 **                                                                        **
 **              3GPP TS 24.301, section 5.4.3.5                           **
 **      Upon receiving the SECURITY MODE REJECT message, the MME  **
 **      shall stop timer T3460 and abort the ongoing procedure    **
 **      that triggered the initiation of the NAS security mode    **
 **      control procedure.                                        **
 **      The MME shall apply the EPS security context in use befo- **
 **      re the initiation of the security mode control procedure, **
 **      if any, to protect any subsequent messages.               **
 **                                                                        **
 **                                                                        **
 ** Inputs:  ue_id:      UE lower layer identifier                  **
 **      Others:    None                                       **
 **                                                                        **
 ** Outputs:     None                                                      **
 **      Return:    RETURNok, RETURNerror                      **
 **      Others:    None                                       **
 **                                                                        **
 ***************************************************************************/
int
emm_proc_security_mode_reject (
  mme_ue_s1ap_id_t ue_id)
{
  emm_data_context_t                     *emm_ctx = NULL;
  int                                     rc = RETURNerror;

  LOG_FUNC_IN (LOG_NAS_EMM);
  LOG_WARNING (LOG_NAS_EMM, "EMM-PROC  - Security mode command not accepted by the UE" "(ue_id=" MME_UE_S1AP_ID_FMT ")\n", ue_id);
  /*
   * Get the UE context
   */

  if (ue_id > 0) {
    emm_ctx = emm_data_context_get (&_emm_data, ue_id);
    DevAssert (emm_ctx );
  }

  if (emm_ctx) {
    /*
     * Stop timer T3460
     */
    LOG_INFO (LOG_NAS_EMM, "EMM-PROC  - Stop timer T3460 (%d)\n", emm_ctx->T3460.id);
    emm_ctx->T3460.id = nas_timer_stop (emm_ctx->T3460.id);
    MSC_LOG_EVENT (MSC_NAS_EMM_MME, "T3460 stopped UE " MME_UE_S1AP_ID_FMT " ", ue_id);
  }

  /*
   * Release retransmission timer paramaters
   */
  security_data_t                        *data = (security_data_t *) (emm_proc_common_get_args (ue_id));

  if (data) {
    FREE_CHECK (data);
  }

  /*
   * Set the key set identifier to its previous value
   */
  if (emm_ctx && emm_ctx->security) {
    /*
     * XXX - Usually, the MME should be able to maintain a current and
     * * * * a non-current EPS security context simultaneously as the UE do.
     * * * * This implementation choose to have only one security context by UE
     * * * * in the MME, thus security mode control procedure is only performed
     * * * * to take into use the first EPS security context created after a
     * * * * successful execution of the EPS authentication procedure
     */
    emm_ctx->security->type = EMM_KSI_NOT_AVAILABLE;
  }

  /*
   * Notify EMM that the authentication procedure failed
   */
  MSC_LOG_TX_MESSAGE (MSC_NAS_EMM_MME, MSC_NAS_EMM_MME, NULL, 0, "EMMREG_COMMON_PROC_REJ ue id " MME_UE_S1AP_ID_FMT " (security mode reject)", ue_id);
  emm_sap_t                               emm_sap = {0};

  emm_sap.primitive = EMMREG_COMMON_PROC_REJ;
  emm_sap.u.emm_reg.ue_id = ue_id;
  emm_sap.u.emm_reg.ctx = emm_ctx;
  rc = emm_sap_send (&emm_sap);
  LOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

/****************************************************************************/
/*********************  L O C A L    F U N C T I O N S  *********************/
/****************************************************************************/



/*
   --------------------------------------------------------------------------
                Timer handlers
   --------------------------------------------------------------------------
*/

/****************************************************************************
 **                                                                        **
 ** Name:    _security_t3460_handler()                                 **
 **                                                                        **
 ** Description: T3460 timeout handler                                     **
 **      Upon T3460 timer expiration, the security mode command    **
 **      message is retransmitted and the timer restarted. When    **
 **      retransmission counter is exceed, the MME shall abort the **
 **      security mode control procedure.                          **
 **                                                                        **
 **              3GPP TS 24.301, section 5.4.3.7, case b                   **
 **                                                                        **
 ** Inputs:  args:      handler parameters                         **
 **      Others:    None                                       **
 **                                                                        **
 ** Outputs:     None                                                      **
 **      Return:    None                                       **
 **      Others:    None                                       **
 **                                                                        **
 ***************************************************************************/
static void                            *
_security_t3460_handler (
  void *args)
{
  LOG_FUNC_IN (LOG_NAS_EMM);
  security_data_t                        *data = (security_data_t *) (args);

  /*
   * Increment the retransmission counter
   */
  data->retransmission_count += 1;
  LOG_WARNING (LOG_NAS_EMM, "EMM-PROC  - T3460 timer expired, retransmission " "counter = %d\n", data->retransmission_count);

  if (data->retransmission_count < SECURITY_COUNTER_MAX) {
    /*
     * Send security mode command message to the UE
     */
    _security_request (data, false);
  } else {
    /*
     * Set the failure notification indicator
     */
    data->notify_failure = true;
    /*
     * Abort the security mode control procedure
     */
    _security_abort (data);
  }

  LOG_FUNC_RETURN (LOG_NAS_EMM, NULL);
}

/*
   --------------------------------------------------------------------------
                MME specific local functions
   --------------------------------------------------------------------------
*/

/****************************************************************************
 **                                                                        **
 ** Name:    _security_request()                                       **
 **                                                                        **
 ** Description: Sends SECURITY MODE COMMAND message and start timer T3460 **
 **                                                                        **
 ** Inputs:  data:      Security mode control internal data        **
 **      is_new:    Indicates whether a new security context   **
 **             has just been taken into use               **
 **      Others:    None                                       **
 **                                                                        **
 ** Outputs:     None                                                      **
 **      Return:    RETURNok, RETURNerror                      **
 **      Others:    T3460                                      **
 **                                                                        **
 ***************************************************************************/
int
_security_request (
  security_data_t * data,
  int is_new)
{
  struct emm_data_context_s              *emm_ctx = NULL;
  emm_sap_t                               emm_sap = {0};
  int                                     rc = RETURNerror;

  LOG_FUNC_IN (LOG_NAS_EMM);
  /*
   * Notify EMM-AS SAP that Security Mode Command message has to be sent
   * to the UE
   */
  emm_sap.primitive = EMMAS_SECURITY_REQ;
  emm_sap.u.emm_as.u.security.guti = NULL;
  emm_sap.u.emm_as.u.security.ue_id = data->ue_id;
  emm_sap.u.emm_as.u.security.msg_type = EMM_AS_MSG_TYPE_SMC;
  emm_sap.u.emm_as.u.security.ksi = data->ksi;
  emm_sap.u.emm_as.u.security.eea = data->eea;
  emm_sap.u.emm_as.u.security.eia = data->eia;
  emm_sap.u.emm_as.u.security.ucs2 = data->ucs2;
  emm_sap.u.emm_as.u.security.uea = data->uea;
  emm_sap.u.emm_as.u.security.uia = data->uia;
  emm_sap.u.emm_as.u.security.gea = data->gea;
  emm_sap.u.emm_as.u.security.umts_present = data->umts_present;
  emm_sap.u.emm_as.u.security.gprs_present = data->gprs_present;
  emm_sap.u.emm_as.u.security.selected_eea = data->selected_eea;
  emm_sap.u.emm_as.u.security.selected_eia = data->selected_eia;

  if (data->ue_id > 0) {
    emm_ctx = emm_data_context_get (&_emm_data, data->ue_id);
  }

  /*
   * Setup EPS NAS security data
   */
  emm_as_set_security_data (&emm_sap.u.emm_as.u.security.sctx, emm_ctx->security, is_new, false);
  MSC_LOG_TX_MESSAGE (MSC_NAS_EMM_MME, MSC_NAS_EMM_MME, NULL, 0, "EMMAS_SECURITY_REQ ue id " MME_UE_S1AP_ID_FMT " ", data->ue_id);
  rc = emm_sap_send (&emm_sap);

  if (rc != RETURNerror) {
    if (emm_ctx->T3460.id != NAS_TIMER_INACTIVE_ID) {
      /*
       * Re-start T3460 timer
       */
      emm_ctx->T3460.id = nas_timer_restart (emm_ctx->T3460.id);
      LOG_INFO (LOG_NAS_EMM, "EMM-PROC  - Restarted Timer T3460 (%d) expires in %ld seconds\n", emm_ctx->T3460.id, emm_ctx->T3460.sec);
      MSC_LOG_EVENT (MSC_NAS_EMM_MME, "T3460 restarted UE " MME_UE_S1AP_ID_FMT " ", data->ue_id);
      AssertFatal(NAS_TIMER_INACTIVE_ID != emm_ctx->T3460.id, "Failed to restart T3460");
    } else {
      /*
       * Start T3460 timer
       */
      emm_ctx->T3460.id = nas_timer_start (emm_ctx->T3460.sec, _security_t3460_handler, data);
      LOG_INFO (LOG_NAS_EMM, "EMM-PROC  - Started Timer T3460 (%d) expires in %ld seconds\n", emm_ctx->T3460.id, emm_ctx->T3460.sec);
      MSC_LOG_EVENT (MSC_NAS_EMM_MME, "T3460 started UE " MME_UE_S1AP_ID_FMT " ", data->ue_id);
      AssertFatal(NAS_TIMER_INACTIVE_ID != emm_ctx->T3460.id, "Failed to start T3460");
    }

  }

  LOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

/****************************************************************************
 **                                                                        **
 ** Name:    _security_abort()                                         **
 **                                                                        **
 ** Description: Aborts the security mode control procedure currently in   **
 **      progress                                                  **
 **                                                                        **
 ** Inputs:  args:      Security mode control data to be released  **
 **      Others:    None                                       **
 **                                                                        **
 ** Outputs:     None                                                      **
 **      Return:    RETURNok, RETURNerror                      **
 **      Others:    T3460                                      **
 **                                                                        **
 ***************************************************************************/
static int
_security_abort (
  void *args)
{
  LOG_FUNC_IN (LOG_NAS_EMM);
  struct emm_data_context_s              *emm_ctx = NULL;
  int                                     rc = RETURNerror;
  security_data_t                        *data = (security_data_t *) (args);

  if (data) {
    unsigned int                            ue_id = data->ue_id;
    int                                     notify_failure = data->notify_failure;

    LOG_WARNING (LOG_NAS_EMM, "EMM-PROC  - Abort security mode control procedure " "(ue_id=" MME_UE_S1AP_ID_FMT ")\n", ue_id);

    if (data->ue_id > 0) {
      emm_ctx = emm_data_context_get (&_emm_data, data->ue_id);
    }

    /*
     * Stop timer T3460
     */
    if (emm_ctx->T3460.id != NAS_TIMER_INACTIVE_ID) {
      LOG_INFO (LOG_NAS_EMM, "EMM-PROC  - Stop timer T3460 (%d)\n", emm_ctx->T3460.id);
      emm_ctx->T3460.id = nas_timer_stop (emm_ctx->T3460.id);
      MSC_LOG_EVENT (MSC_NAS_EMM_MME, "T3460 stopped UE " MME_UE_S1AP_ID_FMT " ", ue_id);
    }

    /*
     * Release retransmission timer paramaters
     */
    FREE_CHECK (data);

    /*
     * Notify EMM that the security mode control procedure failed
     */
    if (notify_failure) {
      MSC_LOG_TX_MESSAGE (MSC_NAS_EMM_MME, MSC_NAS_EMM_MME, NULL, 0, "EMMREG_COMMON_PROC_REJ ue id " MME_UE_S1AP_ID_FMT " (security abort)", data->ue_id);
      emm_sap_t                               emm_sap = {0};

      emm_sap.primitive = EMMREG_COMMON_PROC_REJ;
      emm_sap.u.emm_reg.ue_id = ue_id;
      rc = emm_sap_send (&emm_sap);
    } else {
      rc = RETURNok;
    }
  }

  LOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}


/****************************************************************************
 **                                                                        **
 ** Name:    _security_select_algorithms()                                 **
 **                                                                        **
 ** Description: Select int and enc algorithms based on UE capabilities and**
 **      MME capabilities and MME preferences                              **
 **                                                                        **
 ** Inputs:  ue_eia:      integrity algorithms supported by UE             **
 **          ue_eea:      ciphering algorithms supported by UE             **
 **                                                                        **
 ** Outputs: mme_eia:     integrity algorithms supported by MME            **
 **          mme_eea:     ciphering algorithms supported by MME            **
 **                                                                        **
 **      Return:    RETURNok, RETURNerror                                  **
 **      Others:    None                                                   **
 **                                                                        **
 ***************************************************************************/
static int
_security_select_algorithms (
  const int ue_eiaP,
  const int ue_eeaP,
  int *const mme_eiaP,
  int *const mme_eeaP)
{
  LOG_FUNC_IN (LOG_NAS_EMM);
  int                                     preference_index;

  *mme_eiaP = NAS_SECURITY_ALGORITHMS_EIA0;
  *mme_eeaP = NAS_SECURITY_ALGORITHMS_EEA0;

  for (preference_index = 0; preference_index < 8; preference_index++) {
    if (ue_eiaP & (0x80 >> _emm_data.conf.prefered_integrity_algorithm[preference_index])) {
      LOG_DEBUG (LOG_NAS_EMM, "Selected  NAS_SECURITY_ALGORITHMS_EIA%d (choice num %d)\n", _emm_data.conf.prefered_integrity_algorithm[preference_index], preference_index);
      *mme_eiaP = _emm_data.conf.prefered_integrity_algorithm[preference_index];
      break;
    }
  }

  for (preference_index = 0; preference_index < 8; preference_index++) {
    if (ue_eeaP & (0x80 >> _emm_data.conf.prefered_ciphering_algorithm[preference_index])) {
      LOG_DEBUG (LOG_NAS_EMM, "Selected  NAS_SECURITY_ALGORITHMS_EEA%d (choice num %d)\n", _emm_data.conf.prefered_ciphering_algorithm[preference_index], preference_index);
      *mme_eeaP = _emm_data.conf.prefered_ciphering_algorithm[preference_index];
      break;
    }
  }

  LOG_FUNC_RETURN (LOG_NAS_EMM, RETURNok);
}