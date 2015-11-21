/******************************************************************************
 *
 * Module Name: tbgetall - Get all required ACPI tables
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
#include <acpi/actables.h>

#define _COMPONENT          ACPI_TABLES
ACPI_MODULE_NAME("tbgetall");

/* Local prototypes */
static acpi_status
acpi_tb_get_primary_table(struct acpi_pointer *address,
			  struct acpi_table_desc *table_info);

static acpi_status
acpi_tb_get_secondary_table(struct acpi_pointer *address,
			    acpi_string signature,
			    struct acpi_table_desc *table_info);

/*******************************************************************************
 *
 * FUNCTION:    acpi_tb_get_primary_table
 *
 * PARAMETERS:  Address             - Physical address of table to retrieve
 *              *table_info         - Where the table info is returned
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Maps the physical address of table into a logical address
 *
 ******************************************************************************/
/**ltl
 * 功能:获取主表
 * 参数:
 * 返回值:
 * 说明:主表是在全局变量acpi_gbl_table_data中标记了ACPI_TABLE_PRIMARY的表，包括FADT、FACS、PSDT表
 *	获取到表的过程:1.获取表头；2.校验表签名；3.获取表体；4.将表的描述符信息插入到全局列表中
 */
static acpi_status
acpi_tb_get_primary_table(struct acpi_pointer *address,
			  struct acpi_table_desc *table_info)
{
	acpi_status status;
	struct acpi_table_header header;

	ACPI_FUNCTION_TRACE(tb_get_primary_table);

	/* Ignore a NULL address in the RSDT */

	if (!address->pointer.value) {
		return_ACPI_STATUS(AE_OK);
	}

	/* Get the header in order to get signature and table size */
	/*1.获取表头*/
	status = acpi_tb_get_table_header(address, &header);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Clear the table_info */
	
	ACPI_MEMSET(table_info, 0, sizeof(struct acpi_table_desc));

	/*
	 * Check the table signature and make sure it is recognized.
	 * Also checks the header checksum
	 */
	/*2.识别表，同时对checksum进行验证*/
	table_info->pointer = &header;
	status = acpi_tb_recognize_table(table_info, ACPI_TABLE_PRIMARY);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Get the entire table */
	/* 3.获取整张表，表头与表体 */
	status = acpi_tb_get_table_body(address, &header, table_info);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Install the table */
	/* 4.将ACPI表的描述信息插入到全局列表中 */
	status = acpi_tb_install_table(table_info);
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_tb_get_secondary_table
 *
 * PARAMETERS:  Address             - Physical address of table to retrieve
 *              *table_info         - Where the table info is returned
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Maps the physical address of table into a logical address
 *
 ******************************************************************************/
/**ltl
 * 功能:从内存中读取表数
 * 参数:
 * 返回值:
 * 说明:获取表数据的步骤:1.读取表头；2.根据签名识别表；3.获取表体；4.将表插入到全局数据列表中。
 */
static acpi_status
acpi_tb_get_secondary_table(struct acpi_pointer *address,
			    acpi_string signature,
			    struct acpi_table_desc *table_info)
{
	acpi_status status;
	struct acpi_table_header header;

	ACPI_FUNCTION_TRACE_STR(tb_get_secondary_table, signature);

	/* Get the header in order to match the signature */
	/* 1.获取表头 */
	status = acpi_tb_get_table_header(address, &header);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Signature must match request */

	if (!ACPI_COMPARE_NAME(header.signature, signature)) {
		ACPI_ERROR((AE_INFO,
			    "Incorrect table signature - wanted [%s] found [%4.4s]",
			    signature, header.signature));
		return_ACPI_STATUS(AE_BAD_SIGNATURE);
	}

	/*
	 * Check the table signature and make sure it is recognized.
	 * Also checks the header checksum
	 */
	table_info->pointer = &header;
	/* 2.按签名识别表 */
	status = acpi_tb_recognize_table(table_info, ACPI_TABLE_SECONDARY);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Get the entire table */
	/* 3.获取表体 */
	status = acpi_tb_get_table_body(address, &header, table_info);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Install the table */
	/* 4.将表插入到全局数据区域中 */
	status = acpi_tb_install_table(table_info);
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_tb_get_required_tables
 *
 * PARAMETERS:  None
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Load and validate tables other than the RSDT.  The RSDT must
 *              already be loaded and validated.
 *
 *              Get the minimum set of ACPI tables, namely:
 *
 *              1) FADT (via RSDT in loop below)
 *              2) FACS (via FADT)
 *              3) DSDT (via FADT)
 *
 ******************************************************************************/
/**ltl
 * 功能:解析ACPI表:FADT、FACS、DSDT表
 * 参数:
 * 返回值:
 * 说明:
 */
acpi_status acpi_tb_get_required_tables(void)
{
	acpi_status status = AE_OK;
	u32 i;
	struct acpi_table_desc table_info;
	struct acpi_pointer address;

	ACPI_FUNCTION_TRACE(tb_get_required_tables);

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, "%d ACPI tables in RSDT\n",
			  acpi_gbl_rsdt_table_count));
	/*ACPI_PHYSICAL_POINTER | ACPI_LOGICAL_ADDRESSING*/
	address.pointer_type = acpi_gbl_table_flags | ACPI_LOGICAL_ADDRESSING;

	/*
	 * Loop through all table pointers found in RSDT.
	 * This will NOT include the FACS and DSDT - we must get
	 * them after the loop.
	 *
	 * The only tables we are interested in getting here is the FADT and
	 * any SSDTs.
	 */
	/* acpi_gbl_rsdt_table_count=15 */
	for (i = 0; i < acpi_gbl_rsdt_table_count; i++) {

		/* Get the table address from the common internal XSDT */

		address.pointer.value = acpi_gbl_XSDT->table_offset_entry[i];

		/*
		 * Get the tables needed by this subsystem (FADT and any SSDTs).
		 * NOTE: All other tables are completely ignored at this time.
		 */
		/* 获取ACPI系统主要的几张表: FADT表、FACS表、PSDT表 */
		status = acpi_tb_get_primary_table(&address, &table_info);
		if ((status != AE_OK) && (status != AE_TABLE_NOT_SUPPORTED)) {
			ACPI_WARNING((AE_INFO,
				      "%s, while getting table at %8.8X%8.8X",
				      acpi_format_exception(status),
				      ACPI_FORMAT_UINT64(address.pointer.
							 value)));
		}
	}

	/* We must have a FADT to continue */
	/* 如果acpi_gbl_FADT为NULL,说明acpi_tb_get_primary_table流程没有解析到FADT表 */
	if (!acpi_gbl_FADT) {
		ACPI_ERROR((AE_INFO, "No FADT present in RSDT/XSDT"));
		return_ACPI_STATUS(AE_NO_ACPI_TABLES);
	}

	/*
	 * Convert the FADT to a common format.  This allows earlier revisions of
	 * the table to coexist with newer versions, using common access code.
	 */
	/* 转换FADT表(ACPI 1.0版本的才需要转换) */
	status = acpi_tb_convert_table_fadt();
	if (ACPI_FAILURE(status)) {
		ACPI_ERROR((AE_INFO,
			    "Could not convert FADT to internal common format"));
		return_ACPI_STATUS(status);
	}

	/* Get the FACS (Pointed to by the FADT) */
	/* 获取FACS的首地址(在FADT表中) */
	address.pointer.value = acpi_gbl_FADT->xfirmware_ctrl;
	/* 获取FACS表数据 */
	status = acpi_tb_get_secondary_table(&address, FACS_SIG, &table_info);
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status,
				"Could not get/install the FACS"));
		return_ACPI_STATUS(status);
	}

	/*
	 * Create the common FACS pointer table
	 * (Contains pointers to the original table)
	 */
	/* 转换FACS表 */
	status = acpi_tb_build_common_facs(&table_info);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Get/install the DSDT (Pointed to by the FADT) */
	/* 获取DSDT首地址 */
	address.pointer.value = acpi_gbl_FADT->Xdsdt;
	/* 获取DSDT表数据 */
	status = acpi_tb_get_secondary_table(&address, DSDT_SIG, &table_info);
	if (ACPI_FAILURE(status)) {
		ACPI_ERROR((AE_INFO, "Could not get/install the DSDT"));
		return_ACPI_STATUS(status);
	}

	/* Set Integer Width (32/64) based upon DSDT revision */

	acpi_ut_set_integer_width(acpi_gbl_DSDT->revision);

	/* Dump the entire DSDT */

	ACPI_DEBUG_PRINT((ACPI_DB_TABLES,
			  "Hex dump of entire DSDT, size %d (0x%X), Integer width = %d\n",
			  acpi_gbl_DSDT->length, acpi_gbl_DSDT->length,
			  acpi_gbl_integer_bit_width));

	ACPI_DUMP_BUFFER(ACPI_CAST_PTR(u8, acpi_gbl_DSDT),
			 acpi_gbl_DSDT->length);

	/* Always delete the RSDP mapping, we are done with it */

	acpi_tb_delete_tables_by_type(ACPI_TABLE_ID_RSDP);
	return_ACPI_STATUS(status);
}
