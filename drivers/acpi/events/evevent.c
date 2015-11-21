/******************************************************************************
 *
 * Module Name: evevent - Fixed Event handling and dispatch
 *
 *****************************************************************************/

/*
 * Copyright (C) 2000 - 2006, R. Byron Moore
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    substantially similar to the "NO WARRANTY" disclaimer below
 *    ("Disclaimer") and any redistribution must be conditioned upon
 *    including a substantially similar Disclaimer requirement for further
 *    binary redistribution.
 * 3. Neither the names of the above-listed copyright holders nor the names
 *    of any contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES.
 */

#include <acpi/acpi.h>
#include <acpi/acevents.h>

#define _COMPONENT          ACPI_EVENTS
ACPI_MODULE_NAME("evevent");

/* Local prototypes */
static acpi_status acpi_ev_fixed_event_initialize(void);

static u32 acpi_ev_fixed_event_dispatch(u32 event);

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_initialize_events
 *
 * PARAMETERS:  None
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Initialize global data structures for ACPI events (Fixed, GPE)
 *
 ******************************************************************************/
/**ltl
 * ¹¦ÄÜ: ³õÊ¼»¯ACPIÄ£¿éµÄÊÂ¼þ(Ïà¹Ø¼Ä´æÆ÷)
 * ²ÎÊý:
 * ·µ»ØÖµ:
 * ËµÃ÷:
 */
acpi_status acpi_ev_initialize_events(void)
{
	acpi_status status;

	ACPI_FUNCTION_TRACE(ev_initialize_events);

	/* Make sure we have ACPI tables */

	if (!acpi_gbl_DSDT) {
		ACPI_WARNING((AE_INFO, "No ACPI tables present!"));
		return_ACPI_STATUS(AE_NO_ACPI_TABLES);
	}

	/*
	 * Initialize the Fixed and General Purpose Events. This is done prior to
	 * enabling SCIs to prevent interrupts from occurring before the handlers are
	 * installed.
	 */
	/* ³õÊ¼»¯¹Ì¶¨ºÍÍ¨¹ýÊÂ¼þ */
	status = acpi_ev_fixed_event_initialize();
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status,
				"Unable to initialize fixed events"));
		return_ACPI_STATUS(status);
	}
	/* Ê¹ÄÜGPE¿ØÖÆ¿é¡¢°²×°ÖÐ¶Ï´¦Àíº¯Êý¡¢´´½¨GPE0/GPE1¿ØÖÆ¿éÐÅÏ¢ */
	status = acpi_ev_gpe_initialize();
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status,
				"Unable to initialize general purpose events"));
		return_ACPI_STATUS(status);
	}

	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_install_fadt_gpes
 *
 * PARAMETERS:  None
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Completes initialization of the FADT-defined GPE blocks
 *              (0 and 1). This causes the _PRW methods to be run, so the HW
 *              must be fully initialized at this point, including global lock
 *              support.
 *
 ******************************************************************************/

acpi_status acpi_ev_install_fadt_gpes(void)
{
	acpi_status status;

	ACPI_FUNCTION_TRACE(ev_install_fadt_gpes);

	/* Namespace must be locked */

	status = acpi_ut_acquire_mutex(ACPI_MTX_NAMESPACE);
	if (ACPI_FAILURE(status)) {
		return (status);
	}

	/* FADT GPE Block 0 */

	(void)acpi_ev_initialize_gpe_block(acpi_gbl_fadt_gpe_device,
					   acpi_gbl_gpe_fadt_blocks[0]);

	/* FADT GPE Block 1 */

	(void)acpi_ev_initialize_gpe_block(acpi_gbl_fadt_gpe_device,
					   acpi_gbl_gpe_fadt_blocks[1]);

	(void)acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);
	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_install_xrupt_handlers
 *
 * PARAMETERS:  None
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Install interrupt handlers for the SCI and Global Lock
 *
 ******************************************************************************/
/**ltl
 * ¹¦ÄÜ: ACPIÄ£¿é°²×°ÖÐ¶Ï´¦Àíº¯Êý
 * ²ÎÊý:
 * ·µ»ØÖµ:
 * ËµÃ÷:
 */
acpi_status acpi_ev_install_xrupt_handlers(void)
{
	acpi_status status;

	ACPI_FUNCTION_TRACE(ev_install_xrupt_handlers);

	/* Install the SCI handler */
	/* °²×°SCIÖÐ¶Ï */
	status = acpi_ev_install_sci_handler();
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status,
				"Unable to install System Control Interrupt handler"));
		return_ACPI_STATUS(status);
	}

	/* Install the handler for the Global Lock */
	/* °²×°È«¾ÖËø´¦Àí¾ä±ú */
	status = acpi_ev_init_global_lock_handler();
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status,
				"Unable to initialize Global Lock handler"));
		return_ACPI_STATUS(status);
	}

	acpi_gbl_events_initialized = TRUE;
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_fixed_event_initialize
 *
 * PARAMETERS:  None
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Install the fixed event handlers and enable the fixed events.
 *
 ******************************************************************************/
/**ltl
 * ¹¦ÄÜ: ³õÊ¼»¯ÊÂ¼þ¼Ä´æÆ÷
 * ²ÎÊý:
 * ·µ»ØÖµ:
 * ËµÃ÷: ÊÂ¼þÀàÐÍ:ACPI_EVENT_PMTIMER¡¢ACPI_EVENT_GLOBAL¡¢ACPI_EVENT_POWER_BUTTON¡¢ACPI_EVENT_SLEEP_BUTTON¡¢ACPI_EVENT_RTC¢
 */
static acpi_status acpi_ev_fixed_event_initialize(void)
{
	acpi_native_uint i;
	acpi_status status;

	/*
	 * Initialize the structure that keeps track of fixed event handlers
	 * and enable the fixed events.
	 */
	for (i = 0; i < ACPI_NUM_FIXED_EVENTS; i++) {
		acpi_gbl_fixed_event_handlers[i].handler = NULL;
		acpi_gbl_fixed_event_handlers[i].context = NULL;

		/* Enable the fixed event */
		/* °ÑÊÂ¼þ¼Ä´æÆ÷ÖµÏàÓ¦µÄÊÂ¼þÎ»Çå0 */
		if (acpi_gbl_fixed_event_info[i].enable_register_id != 0xFF) {
			/* ÍùÎå¸öÊ¹ÄÜ¼Ä´æÆ÷Ð´Èë0£¬¿ªÆô´Ë¼Ä´æÆ÷ */
			status =
			    acpi_set_register(acpi_gbl_fixed_event_info[i].enable_register_id,
			    		0, ACPI_MTX_LOCK);
			if (ACPI_FAILURE(status)) {
				return (status);
			}
		}
	}

	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_fixed_event_detect
 *
 * PARAMETERS:  None
 *
 * RETURN:      INTERRUPT_HANDLED or INTERRUPT_NOT_HANDLED
 *
 * DESCRIPTION: Checks the PM status register for active fixed events
 *
 ******************************************************************************/

u32 acpi_ev_fixed_event_detect(void)
{
	u32 int_status = ACPI_INTERRUPT_NOT_HANDLED;
	u32 fixed_status;
	u32 fixed_enable;
	acpi_native_uint i;

	ACPI_FUNCTION_NAME(ev_fixed_event_detect);

	/*
	 * Read the fixed feature status and enable registers, as all the cases
	 * depend on their values.  Ignore errors here.
	 */
	/* ¶ÁÈ¡PM1¼Ä´æÆ÷µÄ×´Ì¬ÖµºÍÊ¹ÄÜÖµ */
	(void)acpi_hw_register_read(ACPI_MTX_DO_NOT_LOCK,
				    ACPI_REGISTER_PM1_STATUS, &fixed_status);
	(void)acpi_hw_register_read(ACPI_MTX_DO_NOT_LOCK,
				    ACPI_REGISTER_PM1_ENABLE, &fixed_enable);

	ACPI_DEBUG_PRINT((ACPI_DB_INTERRUPTS,
			  "Fixed Event Block: Enable %08X Status %08X\n",
			  fixed_enable, fixed_status));

	/*
	 * Check for all possible Fixed Events and dispatch those that are active
	 */
	/* ·Ö·¢ÊÂ¼þ:ACPI_EVENT_PMTIMER¡¢ACPI_EVENT_GLOBAL¡¢ACPI_EVENT_POWER_BUTTON¡¢ACPI_EVENT_SLEEP_BUTTON¡¢ACPI_EVENT_RTC */
	for (i = 0; i < ACPI_NUM_FIXED_EVENTS; i++) {

		/* Both the status and enable bits must be on for this event */

		if ((fixed_status & acpi_gbl_fixed_event_info[i].status_bit_mask)
		    && (fixed_enable & acpi_gbl_fixed_event_info[i].enable_bit_mask)) 
		{
			/* Found an active (signalled) event */

			int_status |= acpi_ev_fixed_event_dispatch((u32) i);
		}
	}

	return (int_status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_fixed_event_dispatch
 *
 * PARAMETERS:  Event               - Event type
 *
 * RETURN:      INTERRUPT_HANDLED or INTERRUPT_NOT_HANDLED
 *
 * DESCRIPTION: Clears the status bit for the requested event, calls the
 *              handler that previously registered for the event.
 *
 ******************************************************************************/
/**ltl
 * ¹¦ÄÜ:·Ö·¢¹Ì¶¨ÊÂ¼þ
 * ²ÎÊý:
 * ·µ»ØÖµ:
 * ËµÃ÷:
 */
static u32 acpi_ev_fixed_event_dispatch(u32 event)
{

	ACPI_FUNCTION_ENTRY();

	/* Clear the status bit */
	/* Çå¿Õ¼Ä´æÆ÷µÄÖµ */
	(void)acpi_set_register(acpi_gbl_fixed_event_info[event].status_register_id, 1, ACPI_MTX_DO_NOT_LOCK);

	/*
	 * Make sure we've got a handler.  If not, report an error.
	 * The event is disabled to prevent further interrupts.
	 */
	if (NULL == acpi_gbl_fixed_event_handlers[event].handler) {
		(void)acpi_set_register(acpi_gbl_fixed_event_info[event].enable_register_id, 
					0, ACPI_MTX_DO_NOT_LOCK);

		ACPI_ERROR((AE_INFO,
			    "No installed handler for fixed event [%08X]",event));

		return (ACPI_INTERRUPT_NOT_HANDLED);
	}

	/* Invoke the Fixed Event handler */
	/* ½«ÊÂ¼þ·¢·Ö·¢¸ø¾ßÌåµÄÇý¶¯³ÌÐò(acpi_gbl_fixed_event_handlersÓÉ¾ßÌåµÄÇý¶¯µ÷ÓÃº¯Êýacpi_install_fixed_event_handlerÀ´ÉèÖÃ) */
	return ((acpi_gbl_fixed_event_handlers[event].handler) (acpi_gbl_fixed_event_handlers[event].context));
}
