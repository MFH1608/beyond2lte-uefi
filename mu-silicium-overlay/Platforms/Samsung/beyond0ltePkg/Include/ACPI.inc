#
# ACPI tablolari hazir ama .aml'ler henuz derlenmedi.
#
# Etkinlestirmek icin (WSL):
#   sudo apt install -y acpica-tools
#   cp -r <proje>/silicium-acpi-overlay/SoCs ~/Mu-Silicium/Silicium-ACPI/
#   bash <proje>/silicium-acpi-overlay/derle.sh
# Sonra asagidaki blogun basindaki '#' isaretlerini kaldir.
#
# .aml dosyalari yokken bu blok acik olursa build su hatayla patlar:
#   error 000E: File/directory not found in workspace / S5E9820/APIC.aml is not found
#
#FILE FREEFORM = 7E374E25-8E01-4FEE-87F2-390C23C606CD {
#    SECTION RAW = S5E9820/APIC.aml
#    SECTION RAW = S5E9820/DSDT_minimal.aml
#    SECTION RAW = Common/SSDT.aml
#    SECTION RAW = S5E9820/FACP.aml
#    SECTION RAW = S5E9820/GTDT.aml
#    SECTION RAW = S5E9820/PPTT.aml
#    SECTION UI = "AcpiTables"
#}
