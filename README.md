# 5g_saveback_2021_0301
这是一份备份文件，用于保存20210301版本的5g代码。




注意：

1：基本编译流程，请参照秀哥之前提供的OAI环境搭建_2021_0617.docx文档。

2：由于国内环境等原因，请用这个build_help替换cmake_targets/tools中的文件。
该文件用于控制编译项目所需的几个组件，通过替换可以修改其下载路径和编译镜像源。

3：请把提供的工具protobuf-c.zip，protobuf-cpp-3.3.0.tar.gz两个下载后解压，放置在ubuntu的/tmp目录下。请把压缩包和解压后的文件夹都放进去。

4：如果遇到nr-softmodem编译不过的情况，是因为在编译时会下载一个新版本的NGAP_R15,这个模块可能会编译不过，因为其文件没有定义一个指针结构体asn_PER_memb_NGAP_OCTET_STRING_CONTAINING_PDUSessionResourceReleaseResponseTransfer__constr_15。
已经上传了旧版本的代码，请把对应结构体定义加到文件/cmake_targets/ranbuild/build/CmakeFiles/NGAP_ProtocolExtensionField.c中:

static asn_per_constraints_t asn_PER_memb_NGAP_OCTET_STRING_CONTAINING_PDUSessionResourceReleaseResponseTransfer__constr_15 CC_NOTUSED = {
	{ APC_UNCONSTRAINED,	-1, -1,  0,  0 },
	{ APC_SEMI_CONSTRAINED,	-1, -1,  0,  0 }	/* (SIZE(0..MAX)) */,
	0, 0	/* No PER value map */
};
